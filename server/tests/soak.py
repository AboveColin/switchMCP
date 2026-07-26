#!/usr/bin/env python3
"""Soak test — hammer the agent for a duration to surface leaks/wedges.

Usage (real console):
    SWITCH_HOST=... SWITCH_TOKEN=... python tests/soak.py --minutes 1440

Loops screenshot + sysinfo on a fixed cadence, tracking the agent's reported
memory footprint over time (from sysinfo.memory.agent_used) and any errors.
A steadily climbing agent_used across hours indicates a leak. Prints a summary
row every `--report` seconds. Ctrl-C to stop early with a summary.
"""

from __future__ import annotations

import argparse
import asyncio
import sys
import time

sys.path.insert(0, "src")

from switch_mcp.clients.factory import make_client  # noqa: E402


async def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--minutes", type=float, default=60)
    ap.add_argument("--interval", type=float, default=2.0, help="seconds between iterations")
    ap.add_argument("--report", type=float, default=60.0, help="seconds between summary rows")
    args = ap.parse_args()

    c = make_client()
    start = time.monotonic()
    deadline = start + args.minutes * 60
    last_report = start
    iters = errors = 0
    mem_first = mem_last = None

    try:
        while time.monotonic() < deadline:
            try:
                await c.screenshot()
                info, _ = await c.call("sysinfo")
                used = info.get("memory", {}).get("agent_used")
                if used is not None:
                    mem_last = used
                    if mem_first is None:
                        mem_first = used
            except Exception:  # noqa: BLE001
                errors += 1
            iters += 1

            now = time.monotonic()
            if now - last_report >= args.report:
                drift = (mem_last - mem_first) if (mem_first and mem_last) else 0
                print(f"[{int(now - start):>6}s] iters={iters} errors={errors} "
                      f"agent_used={mem_last} drift={drift:+d}B")
                last_report = now
            await asyncio.sleep(args.interval)
    except KeyboardInterrupt:
        pass

    drift = (mem_last - mem_first) if (mem_first and mem_last) else 0
    print(f"\nsoak done: {iters} iters, {errors} errors, memory drift {drift:+d} bytes")
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
