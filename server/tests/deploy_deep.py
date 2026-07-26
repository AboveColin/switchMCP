"""Deploy the deep-tier build: enable allow_hardware in config, push the new
nsp, reboot, then validate cheat/i2c/gpio/debug-event features on hardware."""
import asyncio, os, sys, json, time
sys.path.insert(0, "src")
from switch_mcp.clients.agentd import AgentClient
from switch_mcp.clients.base import ClientError

HOST = "192.168.1.42"
TOKEN = open("../.device-token").read().strip()
NSP = "../agent/switch-agentd.nsp"
REMOTE = "/atmosphere/contents/420000000000AE57/exefs.nsp"
CFG = "/config/switch-agentd/config.ini"
TICO = "01419d0b26f07000"


async def wait_up(t):
    end = time.monotonic() + t
    while time.monotonic() < end:
        try:
            r, w = await asyncio.wait_for(asyncio.open_connection(HOST, 6060), 2); w.close(); return True
        except Exception: await asyncio.sleep(4)
    return False


async def main():
    print("waiting for console to be reachable...", flush=True)
    if not await wait_up(3300):
        print("console never came online"); return
    print("console up — deploying deep-tier build", flush=True)
    c = AgentClient(HOST, 6060, TOKEN)
    cfg = (await c.read_file(CFG)).decode()
    if "allow_hardware" not in cfg:
        cfg += "allow_hardware = true\n"
    else:
        cfg = "\n".join(("allow_hardware = true" if l.strip().startswith("allow_hardware") else l)
                        for l in cfg.splitlines()) + "\n"
    await c.write_file(CFG, cfg.encode())
    print("config: allow_hardware=true written")
    data = open(NSP, "rb").read()
    await c.write_file(REMOTE, data)
    st, _ = await c.call("fs.stat", path=REMOTE)
    print("uploaded nsp:", st.get("size"), "match", st.get("size") == len(data))
    try: await c.call("reboot")
    except Exception: pass
    await c.aclose()
    print("rebooting...")
    await asyncio.sleep(8)
    if not await wait_up(180):
        print("did not come back"); return
    await asyncio.sleep(3)
    c = AgentClient(HOST, 6060, TOKEN)

    async def t(cmd, **p):
        try:
            r, b = await asyncio.wait_for(c.call(cmd, **p), 20); r.pop("id", None)
            return f"PASS {cmd}: " + (f"{len(b)}B " if b else "") + json.dumps(r)[:170]
        except Exception as e:
            return f"FAIL {cmd}: {type(e).__name__}: {e}"

    print(await t("agent.info"))
    print(await t("cheat.status"))
    print(await t("i2c.read", device=2, len=2, reg=0))   # Tmp451 temp sensor
    print(await t("gpio.read", pad=25))                  # Vol+ button
    # non-pausing debug + event poll on a launched game
    await c.call("launch", tid=TICO); await asyncio.sleep(6)
    at, _ = await c.call("debug.attach", pause=False)
    base = int(at.get("main_base", "0x0"), 16)
    print(f"PASS debug.attach: paused={at.get('paused')} main_base={hex(base)} pid={at.get('pid')}")
    print(await t("debug.poll_events"))
    if base:
        r, blob = await c.call("debug.read_mem", addr=base, len=16)
        print(f"PASS debug.read_mem @ {hex(base)}: {blob.hex()}")
    await c.call("debug.detach")
    await c.call("terminate")
    await c.aclose()


asyncio.run(main())
