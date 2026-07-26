"""System control: power, brightness, volume, clocks, time, album.

Includes the destructive power operations, which are gated behind a
confirmation token in guards.py."""

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


# --- power / system control (destructive — client should confirm) ------------


@tool("system")
async def preflight() -> str:
    """Check whether NOW is a safe moment for something irreversible.

    Reports battery, charger, SD free space and whether this is emuMMC (where a
    mistake is recoverable by restoring an SD image) or sysMMC (where it may not
    be), plus a list of concrete warnings.

    Call this before a NAND write, a payload reboot or a long dump. The classic
    way to end up with an unbootable console is a power loss part-way through a
    write that was started at 8% battery."""
    reply, _ = await client().call("preflight")
    reply.pop("id", None)
    return json.dumps(reply, indent=2)


@tool("system")
async def journal(max_bytes: int = 16384) -> str:
    """Read the on-device audit trail of everything that changed the console.

    Every mutating command is appended with its outcome, on the SD card rather
    than in memory — the case you actually need it for is reconstructing what
    happened just before a console stopped booting, when nothing in RAM
    survived."""
    reply, _ = await client().call("journal", max_bytes=max_bytes)
    return reply.get("content", "") or reply.get("note", "")


@tool("system")
async def watchdog() -> str:
    """How many times the console has booted without any client connecting.

    A rising count means the agent is starting but nobody can reach it — or is
    not starting at all. Either way the most recent config or agent change is
    the first thing to undo. Resets as soon as a client authenticates."""
    reply, _ = await client().call("watchdog")
    reply.pop("id", None)
    return json.dumps(reply, indent=2)


@tool("system")
async def reboot(confirm: str | None = None) -> str:
    """Reboot the console. DESTRUCTIVE: interrupts anything running and loses
    unsaved game progress. Call once without `confirm` to get a token."""
    require_confirmation(
        "reboot", "reboot the console now, losing any unsaved game progress", confirm
    )
    await client().call("reboot")
    return "Reboot initiated"


@tool("system")
async def shutdown(confirm: str | None = None) -> str:
    """Power off the console. DESTRUCTIVE: it goes offline until someone
    physically powers it on — you cannot wake it remotely, so this ends the
    session. Call once without `confirm` to get a token."""
    require_confirmation(
        "shutdown",
        "power off the console; it cannot be woken remotely and someone must "
        "press the power button to bring it back",
        confirm,
    )
    await client().call("shutdown")
    return "Shutdown initiated"


@tool("system")
async def reboot_to_payload(confirm: str | None = None) -> str:
    """Reboot into the configured payload (hekate/fusee) for recovery.
    DESTRUCTIVE: interrupts anything running, and the console may stop at a
    payload menu needing physical input. Call once without `confirm` first."""
    require_confirmation(
        "reboot_to_payload",
        "reboot into the payload/recovery menu; the console may sit at a menu "
        "until someone interacts with it physically",
        confirm,
    )
    await client().call("reboot_to_payload")
    return "Rebooting to payload"


@tool("system")
async def set_brightness(level: float) -> str:
    """Set screen brightness, 0.0 (dim) to 1.0 (max)."""
    reply, _ = await client().call("set_brightness", level=level)
    return f"Brightness set to {reply.get('level')}"


@tool("system")
async def disable_auto_sleep() -> str:
    """Disable auto-sleep (handheld + docked → Never). Essential for a headless
    console: sleep drops Wi-Fi and makes the agent unreachable. Persists across
    reboots."""
    reply, _ = await client().call("set_sleep", disable=True)
    return (f"Auto-sleep disabled (handheld_plan={reply.get('handheld_sleep_plan')}, "
            f"console_plan={reply.get('console_sleep_plan')})")


@tool("system")
async def set_sleep_plan(handheld_plan: int, console_plan: int) -> str:
    """Set auto-sleep timeouts explicitly. Handheld plan: 0=1min 1=3min 2=5min
    3=10min 4=30min 5=Never. Console (docked) plan: 0=1h 1=2h 2=3h 3=6h 4=12h
    5=Never."""
    reply, _ = await client().call(
        "set_sleep", disable=False, handheld_plan=handheld_plan, console_plan=console_plan
    )
    return f"Sleep plans set: {reply}"


@tool("system")
async def update_agent(local_nsp_path: str, confirm: str | None = None) -> str:
    """Update the on-device agent: upload a new switch-agentd.nsp to the
    Atmosphère contents path, verify it landed intact, then reboot to load it.
    DESTRUCTIVE (reboots). Call once without `confirm` to get a token.
    If the new build is bad, recover by removing the file from the SD card and
    rebooting (see the Troubleshooting wiki page)."""
    import hashlib

    src = resolve_local_path(local_nsp_path, for_write=False)
    data = src.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    remote = "/atmosphere/contents/420000000000AE57/exefs.nsp"

    require_confirmation(
        "update_agent",
        f"replace the on-device agent with {src.name} ({len(data)} bytes, "
        f"sha256 {digest[:16]}…) and reboot the console",
        confirm,
    )

    await client().write_file(remote, data)

    # Verify before rebooting. A truncated or corrupted upload here means the
    # agent does not come back, and recovery needs physical SD-card access.
    try:
        reply, _ = await client().call("fs.hash", path=remote, algo="sha256")
        remote_digest = reply.get("sha256") or reply.get("hash")
    except ClientError as e:
        if e.code != "unknown_command":
            raise
        remote_digest = None  # older agent without fs.hash

    if remote_digest and remote_digest.lower() != digest:
        return (
            f"ABORTED — upload verification failed.\n"
            f"local sha256:  {digest}\n"
            f"device sha256: {remote_digest}\n"
            f"The console was NOT rebooted, so the running agent is untouched. "
            f"Re-run update_agent to retry the upload."
        )

    verified = "verified" if remote_digest else "UNVERIFIED (agent lacks fs.hash)"
    await client().call("agent.restart")
    return (
        f"Uploaded {len(data)} bytes to {remote} ({verified}); rebooting to load it."
    )


# --- more system controls -----------------------------------------------------


@tool("system")
async def get_brightness() -> str:
    """Read the current screen brightness (0.0–1.0)."""
    reply, _ = await client().call("get_brightness")
    return f"Brightness: {reply.get('level')}"


@tool("system")
async def set_auto_brightness(enable: bool = True) -> str:
    """Enable or disable automatic (ambient) brightness control."""
    reply, _ = await client().call("set_auto_brightness", enable=enable)
    return f"Auto-brightness {'enabled' if reply.get('auto_brightness') else 'disabled'}"


@tool("system")
async def get_volume() -> str:
    """Read the system master output volume (0.0–1.0)."""
    reply, _ = await client().call("get_volume")
    return f"Volume: {reply.get('volume')}"


@tool("system")
async def set_volume(volume: float) -> str:
    """Set the system master output volume (0.0–1.0)."""
    reply, _ = await client().call("set_volume", volume=volume)
    return f"Volume set to {reply.get('volume')}"


@tool("system")
async def set_wireless(enable: bool) -> str:
    """Enable/disable wireless (Wi-Fi) communication. Note: disabling it while
    connected over Wi-Fi will cut off the agent until re-enabled locally."""
    reply, _ = await client().call("set_wireless", enable=enable)
    return f"Wireless {'enabled' if reply.get('wireless_enabled') else 'disabled'}"


@tool("system")
async def get_time() -> str:
    """Read the console's current system time (Unix timestamp)."""
    reply, _ = await client().call("get_time")
    return json.dumps({"unix_time": reply.get("unix_time")})


@tool("system")
async def set_time(unix_time: int) -> str:
    """Set the console's system clock to a Unix timestamp (seconds)."""
    reply, _ = await client().call("set_time", unix_time=unix_time)
    return f"Time set to {reply.get('unix_time')}"


@tool("system")
async def console_info() -> str:
    """Serial number, nickname, theme, and region of the console."""
    reply, _ = await client().call("console_info")
    reply.pop("id", None)
    return json.dumps(reply, indent=2)


@tool("system")
async def fs_copy(from_path: str, to_path: str) -> str:
    """Copy a file on the SD card, on-device (no round-trip through the client)."""
    reply, _ = await client().call("fs.copy", **{"from": from_path, "to": to_path})
    return f"Copied {reply.get('bytes')} bytes: {from_path} -> {to_path}"


# --- album, controllers, clocks ----------------------------------------------


@tool("system")
async def album_list(storage: int = 1) -> str:
    """List capture-album files (screenshots/videos). storage: 1=SD, 0=NAND.
    Each entry includes the address fields needed by album_download."""
    reply, _ = await client().call("album.list", storage=storage)
    return json.dumps({"count": reply.get("count"), "files": reply.get("files", [])}, indent=2)


@tool("system")
async def album_download(
    application_id: str, year: int, month: int, day: int, hour: int, minute: int,
    second: int, id: int = 0, content_type: int = 0, storage: int = 1,
    local_path: str | None = None,
) -> str:
    """Download an album file using the fields from album_list. content_type:
    0=screenshot 1=movie. Screenshots return inline; movies over 1 MiB are
    refused. Pass local_path to save to disk."""
    _, blob = await client().call(
        "album.download", application_id=application_id, year=year, month=month,
        day=day, hour=hour, minute=minute, second=second, id=id,
        content_type=content_type, storage=storage,
    )
    if local_path:
        Path(local_path).expanduser().write_bytes(blob)
        return f"Saved {len(blob)} bytes to {local_path}"
    return f"Downloaded {len(blob)} bytes (pass local_path to save)"


@tool("system")
async def list_controllers() -> str:
    """List connected controllers with battery/charging/power state."""
    reply, _ = await client().call("controllers")
    return json.dumps(reply.get("controllers", []), indent=2)


@tool("system")
async def get_clocks() -> str:
    """Read current CPU/GPU/memory clock rates (Hz)."""
    reply, _ = await client().call("get_clocks")
    reply.pop("id", None)
    return json.dumps(reply, indent=2)
