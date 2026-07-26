"""Server-side safety gates.

Two things the docstrings used to only *ask* for:

1. Destructive operations require an explicit confirmation step. A tool
   docstring saying "confirm with the user" is advice to the model, not a
   control — nothing stopped a confident agent from rebooting the console on
   its own initiative. `require_confirmation` turns the first call into a
   preview that returns a token, so acting requires a second, deliberate call.

2. Local filesystem access is bounded. `fs_download`/`fs_upload` read and write
   arbitrary host paths. Over stdio that is the user's own machine and fine;
   over `SWITCH_MCP_TRANSPORT=streamable-http` it is arbitrary remote file
   read/write, so the roots are restricted and the bind is checked.
"""

from __future__ import annotations

import hashlib
import ipaddress
import os
import time
from pathlib import Path


class Blocked(Exception):
    """A guard refused the operation. Message is written for the model."""


# --- destructive-operation confirmation --------------------------------------

# Confirmation tokens live only in this process and expire, so a token cannot be
# stashed and replayed much later against different device state.
_TOKEN_TTL_SECONDS = 300.0
_pending: dict[str, tuple[str, float]] = {}


def _allow_without_confirmation() -> bool:
    return os.environ.get("SWITCH_MCP_ALLOW_DESTRUCTIVE", "").lower() in (
        "1", "true", "yes",
    )


def _token_for(operation: str, detail: str) -> str:
    seed = f"{operation}|{detail}|{time.time()}|{os.getpid()}"
    return hashlib.sha256(seed.encode()).hexdigest()[:12]


def require_confirmation(operation: str, detail: str, confirm: str | None) -> None:
    """Gate a destructive operation behind an explicit second call.

    Raises `Blocked` carrying a token when `confirm` is missing or stale.
    Returns normally — meaning "proceed" — only on a valid, unexpired token, or
    when SWITCH_MCP_ALLOW_DESTRUCTIVE is set for unattended use.

    `detail` must describe the *specific* effect (which title, which path), so
    the token cannot be reused for a different target.
    """
    if _allow_without_confirmation():
        return

    now = time.time()
    for tok, (_, expiry) in list(_pending.items()):
        if expiry < now:
            del _pending[tok]

    if confirm:
        entry = _pending.get(confirm)
        if entry is None:
            raise Blocked(
                f"Confirmation token '{confirm}' is unknown or expired. Call "
                f"{operation} again with no token to get a fresh one, and check "
                f"with the user before confirming."
            )
        recorded_detail, _ = entry
        if recorded_detail != detail:
            raise Blocked(
                f"Confirmation token does not match this request. It was issued "
                f"for: {recorded_detail}. This call is: {detail}. Request a new "
                f"token for the operation you actually intend."
            )
        del _pending[confirm]
        return

    token = _token_for(operation, detail)
    _pending[token] = (detail, now + _TOKEN_TTL_SECONDS)
    raise Blocked(
        f"DESTRUCTIVE — not executed.\n"
        f"Operation: {operation}\n"
        f"Effect: {detail}\n\n"
        f"Confirm with the user, then repeat the call with confirm=\"{token}\" "
        f"to proceed. Token expires in {int(_TOKEN_TTL_SECONDS)}s."
    )


# --- local filesystem bounds -------------------------------------------------


def _allowed_roots() -> list[Path] | None:
    """Roots host file transfers may touch, or None for unrestricted.

    Unrestricted is the default for stdio, where the server already runs with
    the user's own privileges and a path restriction buys nothing.
    """
    raw = os.environ.get("SWITCH_MCP_LOCAL_ROOTS", "").strip()
    if not raw:
        return None
    return [Path(p).expanduser().resolve() for p in raw.split(os.pathsep) if p]


def resolve_local_path(path: str, *, for_write: bool) -> Path:
    """Resolve a host path, enforcing the configured roots.

    Resolves before comparing so symlinks and `..` cannot escape a root.
    """
    p = Path(path).expanduser()
    # A write target may not exist yet, so resolve the parent and re-attach the
    # name; that still collapses symlinks and `..` in the directory portion.
    if p.exists():
        resolved = p.resolve()
    else:
        resolved = p.parent.resolve() / p.name

    roots = _allowed_roots()
    if roots is None:
        if is_remote_transport() and for_write:
            raise Blocked(
                "Writing host files is disabled over a network transport unless "
                "SWITCH_MCP_LOCAL_ROOTS is set to an explicit allowlist."
            )
        return resolved

    for root in roots:
        if resolved == root or root in resolved.parents:
            return resolved
    allowed = os.pathsep.join(str(r) for r in roots)
    raise Blocked(
        f"Path '{path}' is outside the allowed local roots ({allowed}). "
        f"Set SWITCH_MCP_LOCAL_ROOTS to widen this."
    )


# --- transport binding -------------------------------------------------------


def is_remote_transport() -> bool:
    return os.environ.get("SWITCH_MCP_TRANSPORT", "stdio").lower() != "stdio"


def check_bind_safety() -> None:
    """Refuse to expose the server off-box without deliberate opt-in.

    Every tool here can reboot a console, patch live process memory and move
    files on the host. Bound to a routable address with no authentication, that
    is a remote-control service for anyone on the network.
    """
    if not is_remote_transport():
        return

    host = os.environ.get("SWITCH_MCP_HOST", "127.0.0.1")
    try:
        addr = ipaddress.ip_address(host)
        loopback = addr.is_loopback
    except ValueError:
        # Hostnames (including "localhost") can't be classified reliably.
        loopback = host.lower() in ("localhost", "localhost.localdomain")

    if loopback:
        return

    if os.environ.get("SWITCH_MCP_ALLOW_REMOTE", "").lower() not in ("1", "true", "yes"):
        raise SystemExit(
            f"Refusing to bind {host} with a network transport.\n"
            f"This server can reboot the console, patch process memory and read "
            f"and write host files; there is no authentication on the MCP side.\n"
            f"Bind 127.0.0.1 (default), or set SWITCH_MCP_ALLOW_REMOTE=1 if you "
            f"have put your own authentication in front of it."
        )
