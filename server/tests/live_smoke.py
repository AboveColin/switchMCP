#!/usr/bin/env python3
"""Live smoke test against a real console. NOT run by pytest (needs hardware).

Usage:
    SWITCH_HOST=192.168.1.42 SWITCH_TOKEN=... python tests/live_smoke.py

Exercises every read-only tool and reports pass/fail per command. Does not
touch destructive commands (reboot/uninstall/restore) — those are manual.
"""

from __future__ import annotations

import asyncio
import sys

sys.path.insert(0, "src")

from switch_mcp.clients.factory import make_client  # noqa: E402


async def main() -> int:
    c = make_client()
    checks = [
        ("agent.info", {}),
        ("sysinfo", {}),
        ("running_app", {}),
        ("ps", {}),
        ("titles", {}),
        ("crash_reports", {}),
        ("fs.list", {"path": "/"}),
        ("screenshot", {}),
    ]
    failed = 0
    for cmd, params in checks:
        try:
            reply, blob = await asyncio.wait_for(c.call(cmd, **params), timeout=15)
            detail = f"{len(blob)} bytes binary" if blob else "ok"
            print(f"  PASS  {cmd:<16} {detail}")
        except Exception as e:  # noqa: BLE001 — smoke test wants the message
            print(f"  FAIL  {cmd:<16} {type(e).__name__}: {e}")
            failed += 1

    # Non-destructive input round-trip: nudge a stick and release.
    try:
        await c.call("input", buttons=[], hold_ms=50,
                     sticks={"lx": 0, "ly": 0, "rx": 0, "ry": 0})
        print("  PASS  input            neutral input accepted")
    except Exception as e:  # noqa: BLE001
        print(f"  FAIL  input            {e}")
        failed += 1

    print(f"\n{'ALL PASSED' if not failed else f'{failed} FAILED'}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main()))
