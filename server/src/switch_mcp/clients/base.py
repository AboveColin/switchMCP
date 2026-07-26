"""Client interface shared by the native agentd client and fallbacks.

Every tool in server.py drives the console through this interface. The native
`AgentClient` supports the full command set; the sys-botbase and FTP fallbacks
implement the subset their underlying homebrew exposes and raise
`Unsupported` for the rest.
"""

from __future__ import annotations

import abc
from typing import Any


# What an agent should actually DO about each error code. Without this a tool
# failure reaches the model as an opaque string and the usual response is to
# retry the identical call or give up; both are wrong for most of these.
ERROR_GUIDANCE: dict[str, str] = {
    "tier_denied":
        "The agent is configured below the tier this command needs. This cannot "
        "be worked around from here — no other tool call will succeed either. "
        "Raise `tier` in sd:/config/switch-agentd/config.ini and restart the "
        "agent. Call `capabilities` to see the current tier and what it allows.",
    "disabled":
        "A config sub-gate blocks this even at the invasive tier. The message "
        "names the setting; it needs changing on the SD card plus an agent "
        "restart. Do not retry as-is.",
    "unknown_command":
        "This agent build does not implement that command. Call `capabilities` "
        "for the exact list it supports — the agent may be older than this "
        "server.",
    "bad_path":
        "The path was rejected: it must start with '/' and cannot contain '..'. "
        "Paths are relative to the SD card root, not the host filesystem.",
    "not_found":
        "No such file or directory on the console. Use `fs_list` or `fs_find` "
        "to see what is actually there before retrying.",
    "not_attached":
        "No debug session. Call `debug_attach` first, and remember to "
        "`debug_detach` when finished so the target resumes.",
    "ctx_failed":
        "Thread context access failed. Register WRITES require the thread to be "
        "stopped at a debug event (a trapped exception), not merely a paused "
        "attach. Reads need a paused attach — they fail on a non-pausing one.",
    "mount_failed":
        "Could not mount that save. Check the uid from `list_saves`: an account "
        "save needs the matching user, and some titles have no save at all.",
    "auth":
        "The agent rejected the token. SWITCH_TOKEN must match `token` in "
        "sd:/config/switch-agentd/config.ini.",
    "uncertain":
        "The command reached the console but the reply was lost, so it may or "
        "may not have taken effect. Check the device state before re-issuing — "
        "it was deliberately NOT retried automatically.",
    "frame_too_big":
        "The request JSON exceeded 64 KiB. Split it into smaller calls.",
}

# Guidance for transport-level failures, where there is no protocol error code.
UNREACHABLE_GUIDANCE = (
    "Could not reach the console. It may be asleep (the lockscreen sleeps even "
    "with auto-sleep off, which drops the network), or its IP may have changed. "
    "Note that ping is unreliable on this hardware — it drops ICMP but accepts "
    "TCP. Try `find_console` to rescan the network. If it was recently rebooted, "
    "give it ~30s. If it stays unreachable someone may need to wake it."
)


class ClientError(Exception):
    """A command failed on the device (carries a protocol error code).

    The string form appends actionable guidance where we have any, so a tool
    failure tells the operator what to change rather than just what broke.
    """

    def __init__(self, code: str, message: str):
        hint = ERROR_GUIDANCE.get(code)
        full = f"{code}: {message}"
        if hint:
            full += f"\n\nWhat to do: {hint}"
        super().__init__(full)
        self.code = code
        self.raw_message = message
        self.guidance = hint


class Unsupported(ClientError):
    """The connected transport can't perform this command (fallback mode)."""

    def __init__(self, cmd: str):
        super().__init__("unsupported", f"'{cmd}' is not available in this mode")


class SwitchClient(abc.ABC):
    """Async client to a Switch running a management agent."""

    @abc.abstractmethod
    async def call(
        self, cmd: str, payload: bytes = b"", **params: Any
    ) -> tuple[dict[str, Any], bytes]:
        """Issue one command; return (json_reply, binary_payload)."""

    async def screenshot(self) -> bytes:
        _, blob = await self.call("screenshot")
        return blob

    async def read_file(self, path: str) -> bytes:
        """Default chunked read using fs.read; overridable by transports with a
        more efficient native transfer (e.g. FTP)."""
        parts: list[bytes] = []
        offset = 0
        chunk = 1 << 20
        while True:
            reply, blob = await self.call("fs.read", path=path, offset=offset, len=chunk)
            parts.append(blob)
            offset += len(blob)
            if reply.get("eof"):
                return b"".join(parts)

    async def write_file(self, path: str, data: bytes) -> int:
        chunk = 1 << 20
        offset = 0
        while offset < len(data) or offset == 0:
            piece = data[offset : offset + chunk]
            await self.call("fs.write", payload=piece, path=path, offset=offset)
            offset += len(piece)
            if not piece:
                break
        return len(data)
