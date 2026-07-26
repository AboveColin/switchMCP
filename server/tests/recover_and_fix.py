#!/usr/bin/env python3
"""Wait for the console to be rebooted, then autonomously: keep it awake, push
the new self-healing agent build, reboot into it, and disable auto-sleep so it
stays reachable headless. Run in the background; it drives the whole recovery."""

from __future__ import annotations

import asyncio
import os
import sys
import time

sys.path.insert(0, "src")

from switch_mcp.clients.agentd import AgentClient
from switch_mcp.clients.base import ClientError

HOST = os.environ.get("SWITCH_HOST", "192.168.1.42")
TOKEN = os.environ.get("SWITCH_TOKEN") or open("../.device-token").read().strip()
NSP = "../agent/switch-agentd.nsp"
REMOTE = "/atmosphere/contents/420000000000AE57/exefs.nsp"


async def port_open() -> bool:
    try:
        r, w = await asyncio.wait_for(asyncio.open_connection(HOST, 6060), timeout=2)
        w.close()
        return True
    except (OSError, asyncio.TimeoutError):
        return False


async def wait_up(timeout: float) -> bool:
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if await port_open():
            return True
        await asyncio.sleep(4)
    return False


async def nudge(c: AgentClient) -> None:
    """Send a neutral input to reset the console's auto-sleep idle timer."""
    try:
        await c.call("input", buttons=[], hold_ms=20,
                     sticks={"lx": 0, "ly": 0, "rx": 0, "ry": 0})
    except Exception:
        pass


async def main() -> int:
    print(f"[{time.strftime('%H:%M:%S')}] waiting for console reboot (agent on 6060)...", flush=True)
    if not await wait_up(3300):
        print("timed out waiting for reboot", flush=True)
        return 2
    print(f"[{time.strftime('%H:%M:%S')}] agent up — keeping awake + pushing new build", flush=True)
    await asyncio.sleep(3)

    c = AgentClient(HOST, 6060, TOKEN)
    await nudge(c)  # reset sleep timer immediately
    data = open(NSP, "rb").read()
    await c.write_file(REMOTE, data)
    await nudge(c)
    st, _ = await c.call("fs.stat", path=REMOTE)
    if st.get("size") != len(data):
        print(f"size mismatch {st.get('size')} != {len(data)}", flush=True)
        return 1
    print(f"[{time.strftime('%H:%M:%S')}] uploaded new build ({len(data)}B), rebooting into it", flush=True)
    try:
        await c.call("reboot")
    except (ClientError, ConnectionError, asyncio.IncompleteReadError, OSError):
        pass
    await c.aclose()

    await asyncio.sleep(8)
    if not await wait_up(180):
        print("new build did not come back", flush=True)
        return 2
    print(f"[{time.strftime('%H:%M:%S')}] new agent online — disabling auto-sleep", flush=True)
    await asyncio.sleep(3)

    c2 = AgentClient(HOST, 6060, TOKEN)
    info, _ = await c2.call("agent.info")
    r, _ = await c2.call("set_sleep", disable=True)
    print(f"agent_version={info.get('agent_version')} "
          f"set_sleep -> handheld={r.get('handheld_sleep_plan')} console={r.get('console_sleep_plan')}", flush=True)
    # prove it's fully alive
    _, jpeg = await c2.call("screenshot")
    open("../recovered_screenshot.jpg", "wb").write(jpeg)
    print(f"[{time.strftime('%H:%M:%S')}] DONE: auto-sleep disabled, screenshot saved ({len(jpeg)}B)", flush=True)
    await c2.aclose()
    return 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
