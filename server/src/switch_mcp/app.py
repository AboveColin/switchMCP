"""Shared MCP application object, client accessor, and tool-group gating.

Split out of server.py so the tool modules can import it without importing each
other, and so which groups are registered is decided in one place.

Why gate at all: this server exposes ~93 tools. Most MCP clients degrade well
before that — the model spends its attention picking from a menu instead of
doing the task, and the low-level debugger and cheat tools are irrelevant noise
for the common case of "look at the screen and press a button". Groups let a
session expose the ~30 tools it actually needs.
"""

from __future__ import annotations

import os

from mcp.server.fastmcp import FastMCP

from .clients.base import SwitchClient
from .clients.factory import make_client

# Bind host/port matter only for the network transports (sse/streamable-http);
# they're ignored for stdio. Default to localhost:8730.
mcp = FastMCP(
    "switch",
    host=os.environ.get("SWITCH_MCP_HOST", "127.0.0.1"),
    port=int(os.environ.get("SWITCH_MCP_PORT", "8730")),
)

_client: SwitchClient | None = None


def client() -> SwitchClient:
    global _client
    if _client is None:
        _client = make_client()
    return _client


# --- tool groups --------------------------------------------------------------

# Group names, in rough order of how often a session needs them.
ALL_GROUPS = ("core", "fs", "apps", "system", "debug", "cheat", "hardware")

# What a session gets when SWITCH_MCP_TOOLS is unset. Deliberately excludes the
# deep-tier groups: they are powerful but narrow, and a session that needs them
# knows it and can ask.
DEFAULT_GROUPS = ("core", "fs", "apps", "system")


def enabled_groups() -> tuple[str, ...]:
    """Groups to register, from SWITCH_MCP_TOOLS.

    Accepts a comma-separated list, or "all". Unknown names are ignored rather
    than fatal — a typo should not leave the server with no tools at all.
    """
    raw = os.environ.get("SWITCH_MCP_TOOLS", "").strip().lower()
    if not raw:
        return DEFAULT_GROUPS
    if raw == "all":
        return ALL_GROUPS
    picked = tuple(g for g in (x.strip() for x in raw.split(",")) if g in ALL_GROUPS)
    return picked or DEFAULT_GROUPS


def tool(group: str):
    """Register an MCP tool, but only if its group is enabled.

    Used exactly like `@mcp.tool()`. When the group is off the function is left
    undecorated and simply never reaches the client, so there is no cost beyond
    the import.
    """

    def decorate(fn):
        if group in enabled_groups():
            return mcp.tool()(fn)
        return fn

    return decorate


def addr(a) -> int:
    """Accept an address as an int, a decimal string, or '0x...'."""
    if isinstance(a, int):
        return a
    return int(a, 16) if str(a).lower().startswith("0x") else int(a)
