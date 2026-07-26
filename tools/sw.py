#!/usr/bin/env python3
"""Thin CLI over the switch-agentd client for driving the console from the shell."""
import asyncio, base64, json, os, sys

# Lives in tools/, so the repo root is one level up.
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "server", "src"))
if not os.environ.get("SWITCH_HOST"):
    print("SWITCH_HOST not set (console IP); try: source dev.sh", file=sys.stderr)
    sys.exit(2)
if not os.environ.get("SWITCH_TOKEN"):
    tok = os.path.join(ROOT, ".device-token")
    if os.path.exists(tok):
        os.environ["SWITCH_TOKEN"] = open(tok).read().strip()
from switch_mcp.clients.agentd import AgentClient


async def main():
    c = AgentClient.from_env()
    cmd = sys.argv[1]
    try:
        if cmd == "put":
            local, remote = sys.argv[2], sys.argv[3]
            data = open(local, "rb").read()
            n = await c.write_file(remote, data)
            print(json.dumps({"wrote": n, "to": remote}))
        elif cmd == "screenshot":
            out = sys.argv[2] if len(sys.argv) > 2 else "shot.jpg"
            jpeg = await c.screenshot()
            open(out, "wb").write(jpeg)
            print(json.dumps({"saved": out, "bytes": len(jpeg)}))
        elif cmd == "call":
            # sw.py call <cmd> key=val key=val ...  (values JSON-parsed)
            params = {}
            for kv in sys.argv[3:]:
                k, v = kv.split("=", 1)
                try:
                    params[k] = json.loads(v)
                except json.JSONDecodeError:
                    params[k] = v
            reply, blob = await c.call(sys.argv[2], **params)
            print(json.dumps(reply))
            if blob:
                print(f"[+{len(blob)} bytes binary]", file=sys.stderr)
        else:
            print("usage: put|screenshot|call", file=sys.stderr); sys.exit(2)
    finally:
        await c.aclose()

asyncio.run(main())
