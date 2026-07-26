"""MCP resources and prompts: addressable read-only state and reusable
operator workflows."""

from __future__ import annotations

import base64
import json
from pathlib import Path

from mcp.server.fastmcp import Image

from ..app import addr as _addr
from ..app import client, mcp, tool
from ..clients.base import ClientError
from ..guards import require_confirmation, resolve_local_path
from ..screen import downscale, looks_same, phash


# --- MCP resources (read-only, addressable state) ----------------------------


@mcp.resource("switch://sysinfo", mime_type="application/json")
async def resource_sysinfo() -> str:
    """Live system info as an addressable resource."""
    reply, _ = await client().call("sysinfo")
    reply.pop("id", None)
    return json.dumps(reply, indent=2)


@mcp.resource("switch://screen", mime_type="image/jpeg")
async def resource_screen() -> bytes:
    """The current screen as an addressable JPEG resource."""
    return await client().screenshot()


@mcp.resource("switch://titles", mime_type="application/json")
async def resource_titles() -> str:
    """Installed titles as an addressable resource."""
    reply, _ = await client().call("titles")
    return json.dumps(reply["titles"], indent=2)


# --- MCP prompts (reusable operator workflows) --------------------------------


@mcp.prompt()
def diagnose() -> str:
    """Guide the model through triaging a misbehaving console."""
    return (
        "Diagnose the Switch. Start by calling the `diagnose` tool for a full "
        "snapshot (system info, running app, crash reports, screenshot). If a "
        "crash report exists, fetch it with `get_crash_reports(name=...)` and "
        "map the faulting program ID to a title via `list_titles`. Inspect the "
        "app's files (`fs_list`, `fs_read_text`) and config for the root cause. "
        "Check temperatures and free storage in the sysinfo. Report the cause "
        "and a concrete fix before changing anything."
    )


@mcp.prompt()
def navigate_to(destination: str) -> str:
    """Guide the model to navigate the console UI to a destination."""
    return (
        f"Navigate the Switch UI to: {destination}. Call `capture_screen` to "
        "see the current state, then drive the UI with `tap`/`swipe` (screen is "
        "1280x720) or `send_input`/`send_input_sequence` (buttons: A B X Y L R "
        "ZL ZR PLUS MINUS DUP DDOWN DLEFT DRIGHT). Capture the screen again "
        "after each step to confirm progress. Prefer touch for HOME-menu and "
        "Settings navigation. Stop and re-capture if the screen isn't what you "
        "expected."
    )


@mcp.prompt()
def backup_saves() -> str:
    """Guide the model to back up all game saves."""
    return (
        "Back up save data. Call `list_titles` to enumerate installed titles, "
        "then `backup_save(title_id=...)` for each game title (skip system "
        "titles and apps without saves). Report which titles were backed up and "
        "where (sd:/switch-agentd/saves/<tid>). If a backup fails with "
        "'mount_failed', the title likely has no save data for the primary user."
    )
