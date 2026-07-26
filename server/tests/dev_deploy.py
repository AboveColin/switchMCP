#!/usr/bin/env python3
"""Over-the-air dev loop for switch-agentd.

Pushes a freshly built exefs.nsp to the console THROUGH THE RUNNING AGENT
(its own fs.write) — no FTP, no SD pull — then reboots into the new build,
waits for it to come back, and runs smoke checks. This is the fast iterate
loop: `make` in agent/, then this.

    SWITCH_HOST=... SWITCH_TOKEN=... python tests/dev_deploy.py [--no-reboot] [--no-smoke]

Reads SWITCH_TOKEN from ../.device-token if the env var is unset.
"""

from __future__ import annotations

import argparse
import asyncio
import os
import sys
import time

sys.path.insert(0, "src")

from switch_mcp.clients.agentd import AgentClient  # noqa: E402
from switch_mcp.clients.base import ClientError  # noqa: E402

TITLE_ID = "420000000000AE57"
REMOTE_NSP = f"/atmosphere/contents/{TITLE_ID}/exefs.nsp"
NSP = "../agent/switch-agentd.nsp"


def token() -> str:
    t = os.environ.get("SWITCH_TOKEN")
    if not t:
        t = open("../.device-token").read().strip()
    return t


async def wait_for_port(host: str, port: int, timeout: float) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            r, w = await asyncio.wait_for(asyncio.open_connection(host, port), timeout=2)
            w.close()
            return True
        except (OSError, asyncio.TimeoutError):
            await asyncio.sleep(3)
    return False


async def smoke(c: AgentClient) -> int:
    checks = ["agent.info", "sysinfo", "titles", "fs.list", "screenshot"]
    fails = 0
    for cmd in checks:
        try:
            params = {"path": "/"} if cmd == "fs.list" else {}
            reply, blob = await asyncio.wait_for(c.call(cmd, **params), timeout=20)
            print(f"  PASS  {cmd:<12} {len(blob)}B bin" if blob else f"  PASS  {cmd}")
        except Exception as e:  # noqa: BLE001
            print(f"  FAIL  {cmd:<12} {e}")
            fails += 1
    return fails


async def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--no-reboot", action="store_true", help="upload only, don't reboot")
    ap.add_argument("--no-smoke", action="store_true", help="skip post-reboot smoke test")
    args = ap.parse_args()

    host = os.environ.get("SWITCH_HOST", "192.168.1.42")
    data = open(NSP, "rb").read()
    c = AgentClient(host, 6060, token())

    print(f"→ uploading {len(data)} bytes to {REMOTE_NSP} via agent fs.write")
    await c.write_file(REMOTE_NSP, data)
    st, _ = await c.call("fs.stat", path=REMOTE_NSP)
    if st.get("size") != len(data):
        print(f"✗ size mismatch: remote {st.get('size')} != local {len(data)}")
        return 1
    print(f"✓ uploaded and verified ({st['size']} bytes)")

    if args.no_reboot:
        print("→ --no-reboot: change takes effect on next boot")
        await c.aclose()
        return 0

    print("→ rebooting into new build ...")
    try:
        await c.call("reboot")
    except (ClientError, ConnectionError, asyncio.IncompleteReadError, OSError):
        pass  # connection drops as the console reboots — expected
    await c.aclose()

    print("→ waiting for console to go down then come back ...")
    await asyncio.sleep(8)  # let it actually drop
    if not await wait_for_port(host, 6060, timeout=180):
        print("✗ agent did not come back within 180s")
        return 2
    print("✓ agent back online")

    if args.no_smoke:
        return 0
    await asyncio.sleep(3)
    c2 = AgentClient(host, 6060, token())
    fails = await smoke(c2)
    await c2.aclose()
    print("✓ dev deploy complete" if not fails else f"✗ {fails} smoke check(s) failed")
    return 1 if fails else 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
