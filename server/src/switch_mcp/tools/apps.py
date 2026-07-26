"""Installed titles, running processes, telemetry and crash reports."""

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


# --- telemetry ----------------------------------------------------------------


@tool("apps")
async def system_info() -> str:
    """Firmware, uptime, battery, temperatures, storage, memory, network."""
    reply, _ = await client().call("sysinfo")
    reply.pop("id", None)
    return json.dumps(reply, indent=2)


@tool("apps")
async def list_processes() -> str:
    """List running processes (pid + title ID). Shows what's actually running."""
    reply, _ = await client().call("ps")
    return json.dumps(reply["processes"], indent=2)


@tool("apps")
async def net_info() -> str:
    """The console's network configuration as it sees it: IP, subnet, gateway,
    DNS, and whether wireless is on.

    Useful when the console has become hard to reach — it answers "did the DHCP
    address change?" from the device rather than by scanning. Note this console
    drops ICMP but accepts TCP, so ping failing does not mean it is down."""
    reply, _ = await client().call("net_info")
    reply.pop("id", None)
    return json.dumps(reply, indent=2)


@tool("apps")
async def fatal_reports(name: str = "") -> str:
    """List Atmosphère FATAL reports, or fetch one by name.

    Different from get_crash_reports: fatal reports are written when the system
    itself goes down, not when a single process crashes. Check both when
    investigating an unexplained reboot or black screen."""
    reply, _ = await client().call("fatal_reports", name=name)
    reply.pop("id", None)
    return reply.get("content", "") if name else json.dumps(reply, indent=2)


@tool("apps")
async def get_crash_reports(name: str | None = None) -> str:
    """List Atmosphère crash reports, or fetch one by name for full contents.
    Core of diagnosing crashes/hangs."""
    if name:
        reply, _ = await client().call("crash_reports", name=name)
        return reply.get("content", "")
    reply, _ = await client().call("crash_reports")
    return json.dumps(reply["reports"], indent=2)


@tool("apps")
async def tail_log(path: str | None = None, max_bytes: int = 16384) -> str:
    """Tail a log file on the SD card. Defaults to the agent's own log."""
    kwargs = {"max_bytes": max_bytes}
    if path:
        kwargs["path"] = path
    reply, _ = await client().call("read_log", **kwargs)
    return reply.get("content", "")


# --- apps ---------------------------------------------------------------------


@tool("apps")
async def list_titles() -> str:
    """List installed titles (title ID, name, author, version)."""
    reply, _ = await client().call("titles")
    return json.dumps(reply["titles"], indent=2)


@tool("apps")
async def running_app() -> str:
    """Report the currently running foreground application (title ID + pid)."""
    reply, _ = await client().call("running_app")
    reply.pop("id", None)
    return json.dumps(reply, indent=2)


@tool("apps")
async def get_title_icon(title_id: str) -> Image:
    """Fetch a title's icon as a JPEG image."""
    _, blob = await client().call("title_icon", tid=title_id)
    return Image(data=blob, format="jpeg")


@tool("apps")
async def launch_title(title_id: str) -> str:
    """Launch a title by its 16-hex-digit title ID (see list_titles).

    DO NOT use this for homebrew. `0142b048fd620000` is the hbloader/album
    takeover program ID: launching it directly spawns a process that never
    renders anything, and that dead process then occupies the application slot
    so the normal route (Album applet, or holding R while starting a game) also
    fails — which looks exactly like "hbmenu will not start". Verified on
    hardware: the process reports running=True while the screen stays on the
    home menu.

    If that has already happened, `terminate_title()` with no argument clears
    the stale slot."""
    await client().call("launch", tid=title_id)
    return f"Launched {title_id}"


@tool("apps")
async def terminate_title(title_id: str | None = None) -> str:
    """Close a title by ID, or the current foreground app if no ID is given."""
    kwargs = {"tid": title_id} if title_id else {}
    await client().call("terminate", **kwargs)
    return f"Terminated {title_id or 'foreground app'}"


@tool("apps")
async def uninstall_title(title_id: str, confirm: str | None = None) -> str:
    """Permanently uninstall a title (save data is kept). DESTRUCTIVE. Requires
    allow_nand_write=true in the agent config. Call once without `confirm` to get
    a token, then repeat with it. Installs are out of scope; use DBI/Awoo."""
    require_confirmation(
        "uninstall_title", f"permanently uninstall title {title_id}", confirm
    )
    await client().call("uninstall", tid=title_id)
    return f"Uninstalled {title_id}"


@tool("apps")
async def restart_app(title_id: str) -> str:
    """Terminate and relaunch a title."""
    import asyncio

    await client().call("terminate", tid=title_id)
    await asyncio.sleep(1.0)
    await client().call("launch", tid=title_id)
    return f"Restarted {title_id}"
