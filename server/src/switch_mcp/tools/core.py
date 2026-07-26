"""Connectivity, diagnostics, screen capture and input.

The tools a session almost always needs: is the console reachable, what
is on screen, and press something."""

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


# --- connection / diagnostics ------------------------------------------------


@tool("core")
async def ping() -> str:
    """Check connectivity to the console. Returns agent version and firmware,
    or a clear error if the console is unreachable (asleep? IP changed?)."""
    reply, _ = await client().call("agent.info")
    reply.pop("id", None)
    return json.dumps(reply, indent=2)


@tool("core")
async def capabilities() -> str:
    """What this console's agent will actually permit right now.

    Reports the capability tier (observe = read-only, control = drive the
    console, invasive = NAND/hardware/memory writes), whether it is running on
    emuMMC (where mistakes are recoverable by restoring an SD image) or sysMMC
    (where they may not be), and the exact command list allowed at that tier.

    Check this before planning destructive work: a command absent from the list
    will be refused no matter how it is called, and the fix is a config change
    on the SD card plus an agent restart, not a different tool call."""
    c = client()
    await c.call("agent.info")  # forces the handshake if not yet connected
    caps = getattr(c, "capabilities", {})
    if not caps:
        return json.dumps({"note": "agent did not report capabilities (pre-0.2.0)"})
    summary = dict(caps)
    cmds = summary.pop("commands", None)
    summary["command_count"] = len(cmds) if cmds is not None else None
    summary["commands"] = cmds
    if summary.get("tier") == "invasive" and not summary.get("emummc"):
        summary["warning"] = (
            "invasive tier on sysMMC: destructive writes here can permanently "
            "brick the console and cannot be undone by reflashing the SD card"
        )
    return json.dumps(summary, indent=2)


@tool("core")
async def find_console() -> str:
    """Scan the local network for a console running switch-agentd. Useful when
    the console's IP changed (DHCP). Uses SWITCH_PORT/SWITCH_TOKEN from the env."""
    import os

    from .discovery import scan

    found = await scan(
        port=int(os.environ.get("SWITCH_PORT", "6060")),
        token=os.environ.get("SWITCH_TOKEN", ""),
    )
    if not found:
        return "No console found on the local /24. Is it awake and on this network?"
    return json.dumps(found, indent=2)


@tool("core")
async def diagnose() -> list:
    """One-shot triage snapshot: system info, running app, process list, recent
    crash reports, and a screenshot. Use this first when investigating a problem."""
    c = client()
    info, _ = await c.call("sysinfo")
    running, _ = await c.call("running_app")
    crashes, _ = await c.call("crash_reports")
    try:
        jpeg = await c.screenshot()
    except Exception as e:  # screenshot is best-effort in a triage bundle
        jpeg = None
    for d in (info, running, crashes):
        d.pop("id", None)
    summary = {
        "system": info,
        "running_app": running,
        "crash_reports": crashes.get("reports", []),
    }
    out: list = [json.dumps(summary, indent=2)]
    if jpeg:
        # Triage wants "what is on screen", not pixel detail.
        out.append(Image(data=downscale(jpeg, 0.5), format="jpeg"))
    return out


# --- eyes ---------------------------------------------------------------------


@tool("core")
async def capture_screen(scale: float = 0.5, quality: int = 80) -> Image:
    """Capture the console's current screen as a JPEG.

    Coordinates for `tap`/`swipe` are ALWAYS in native 1280x720 space whatever
    `scale` you pass — scaling changes only the returned picture, never the
    coordinate system.

    `scale` defaults to 0.5 (640x360): that reads menus, text and UI state
    perfectly well at roughly a quarter of the token cost. Use scale=1.0 only
    when you need fine detail, such as small text or a rendering bug."""
    jpeg = await client().screenshot()
    return Image(data=downscale(jpeg, scale, quality), format="jpeg")


@tool("core")
async def stream_screen(frames: int = 5, interval_ms: int = 400,
                        scale: float = 0.4, skip_identical: bool = True) -> list:
    """Capture a short burst of screenshots to 'watch' an animation or transition.

    Returns up to `frames` images (capped at 20) spaced interval_ms apart. With
    `skip_identical` (the default) frames matching the previous one are dropped
    instead of returned, so a burst over a static screen costs one image rather
    than twenty. The leading text says how many were skipped.

    Scaled to 0.4 by default: bursts are for spotting change, not detail."""
    import asyncio

    frames = max(1, min(frames, 20))
    c = client()
    out: list = []
    last_hash = None
    skipped = 0
    for i in range(frames):
        jpeg = await c.screenshot()
        if skip_identical:
            h = phash(jpeg)
            if last_hash is not None and looks_same(last_hash, h):
                skipped += 1
                if i + 1 < frames:
                    await asyncio.sleep(interval_ms / 1000)
                continue
            last_hash = h
        out.append(Image(data=downscale(jpeg, scale), format="jpeg"))
        if i + 1 < frames:
            await asyncio.sleep(interval_ms / 1000)
    summary = f"{len(out)} frame(s) returned"
    if skipped:
        summary += f", {skipped} identical frame(s) skipped"
    return [summary] + out


@tool("core")
async def screen_changed(timeout_ms: int = 5000, poll_ms: int = 300,
                         scale: float = 0.5) -> list:
    """Wait until the screen changes, then return the new screen.

    Much cheaper than polling capture_screen in a loop: frames are compared on
    the server with a perceptual hash, and only the final changed screen comes
    back as an image. Use after pressing a button or launching something to find
    out when the UI has actually settled.

    Returns a text verdict plus, if it changed, one image. If nothing changes
    within timeout_ms it says so and returns no image at all, costing almost
    nothing — which is also how you detect that an input did not register."""
    import asyncio

    c = client()
    baseline = phash(await c.screenshot())
    waited = 0
    while waited < timeout_ms:
        await asyncio.sleep(poll_ms / 1000)
        waited += poll_ms
        jpeg = await c.screenshot()
        if not looks_same(baseline, phash(jpeg)):
            return [f"screen changed after ~{waited}ms",
                    Image(data=downscale(jpeg, scale), format="jpeg")]
    return [f"no change after {timeout_ms}ms (screen is static; the action may "
            f"not have registered)"]


# --- hands --------------------------------------------------------------------


@tool("core")
async def send_input(
    buttons: list[str] | None = None,
    hold_ms: int = 100,
    left_stick_x: float = 0.0,
    left_stick_y: float = 0.0,
    right_stick_x: float = 0.0,
    right_stick_y: float = 0.0,
) -> str:
    """Press/hold controller buttons and/or move sticks, then release.
    Buttons: A B X Y L R ZL ZR PLUS MINUS DUP DDOWN DLEFT DRIGHT LSTICK RSTICK.
    Stick axes are -1.0..1.0 (y+ is up). hold_ms caps at 10000."""
    await client().call(
        "input",
        buttons=buttons or [],
        hold_ms=hold_ms,
        sticks={
            "lx": left_stick_x,
            "ly": left_stick_y,
            "rx": right_stick_x,
            "ry": right_stick_y,
        },
    )
    return "Input sent"


@tool("core")
async def send_input_sequence(steps: list[dict]) -> str:
    """Run a macro of inputs without per-step round-trips. Each step is
    {"buttons": [...], "hold_ms": int, "wait_ms": int}. wait_ms pauses after
    release before the next step. Ideal for navigating menus."""
    import asyncio

    for step in steps:
        await client().call(
            "input",
            buttons=step.get("buttons", []),
            hold_ms=step.get("hold_ms", 100),
            sticks=step.get("sticks", {"lx": 0, "ly": 0, "rx": 0, "ry": 0}),
        )
        wait = step.get("wait_ms", 150)
        if wait:
            await asyncio.sleep(wait / 1000)
    return f"Ran {len(steps)} input step(s)"


@tool("core")
async def wait_event(event: str = "app_change", timeout_ms: int = 10000) -> str:
    """Block on the console until something happens, instead of polling.

    One call replaces a polling loop — and on this transport each poll is a
    Wi-Fi round-trip AND a tool call, so "wait for the game to load" as repeated
    screenshots costs far more than the answer is worth.

    event:
      app_change  (default) EDGE: an application starts, exits or switches
                  after this call begins
      app_start   LEVEL: an application is running. Returns immediately if one
                  already is.
      app_exit    LEVEL: no application is running. Returns immediately if none
                  is.
      button      EDGE: the physical controller is pressed

    app_start/app_exit are level-triggered on purpose. Edge semantics are
    unusable here: the agent serves one command at a time, so you cannot trigger
    a transition while a wait holds the only connection. Launch first, then wait
    for app_start — it answers "is it up yet?" rather than missing the edge.

    Returns fired=true with details, or fired=false on timeout — which is a real
    answer, not an error: it is how you learn an action did not take effect.
    Capped at 20s per call; issue another to keep waiting.

    IMPORTANT: the agent serves one command at a time, so a wait blocks every
    other tool for its duration — including from other sessions. Do not start a
    long wait and then expect to trigger the event with another tool call; that
    call cannot get through until the wait returns. Trigger first, then wait."""
    reply, _ = await client().call("wait_event", event=event, timeout_ms=timeout_ms)
    reply.pop("id", None)
    return json.dumps(reply, indent=2)


@tool("core")
async def read_input() -> str:
    """Read the PHYSICAL controller state — what the human is holding right now.

    Useful to see whether someone is actively using the console before you take
    it over, and to confirm a real controller is connected."""
    reply, _ = await client().call("read_input")
    reply.pop("id", None)
    return json.dumps(reply, indent=2)


@tool("core")
async def record_input(duration_ms: int = 5000) -> str:
    """Record real controller input into a replayable sequence.

    Turns "describe the button presses you want" into "do it once on the
    console and keep it". The returned `sequence` is exactly the shape
    send_input_sequence consumes, so it can be replayed with no editing, and the
    gaps between presses are preserved as wait_ms.

    Sampled every 50ms; capped at 20s."""
    reply, _ = await client().call("record_input", duration_ms=duration_ms)
    reply.pop("id", None)
    return json.dumps(reply, indent=2)


@tool("core")
async def type_text(text: str, key_ms: int = 40) -> str:
    """Type a string via a virtual USB keyboard. Works wherever the system
    software keyboard is open and accepts keyboard input. Unsupported characters
    are skipped. Open the text field first (tap it) before calling this."""
    reply, _ = await client().call("type_text", text=text, key_ms=key_ms)
    return f"Typed {reply.get('typed', 0)} character(s)"


@tool("core")
async def tap(x: int, y: int, duration_ms: int = 50) -> str:
    """Tap the touchscreen at pixel (x, y). Screen is 1280x720 in handheld.
    Many system menus are faster to drive by touch than by D-pad."""
    await client().call("touch", x=x, y=y, duration_ms=duration_ms)
    return f"Tapped ({x}, {y})"


@tool("core")
async def swipe(x: int, y: int, x2: int, y2: int, duration_ms: int = 300) -> str:
    """Swipe the touchscreen from (x, y) to (x2, y2) over duration_ms."""
    await client().call("touch", x=x, y=y, x2=x2, y2=y2, duration_ms=duration_ms)
    return f"Swiped ({x},{y}) -> ({x2},{y2})"
