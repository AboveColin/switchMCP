"""Poll the console's agent port; when it opens, run full live verification.
Exits 0 as soon as verification runs (pass or fail), or 2 if it never appears."""
import asyncio, sys, time, os
sys.path.insert(0, "src")
from switch_mcp.clients.agentd import AgentClient

HOST="192.168.1.42"
TOKEN=open("../.device-token").read().strip()
DEADLINE=time.monotonic()+540  # ~9 min

async def port_open():
    try:
        r,w=await asyncio.wait_for(asyncio.open_connection(HOST,6060),timeout=2)
        w.close(); return True
    except Exception:
        return False

async def verify():
    c=AgentClient(HOST,6060,TOKEN)
    checks=[("agent.info",{}),("sysinfo",{}),("running_app",{}),("ps",{}),
            ("titles",{}),("crash_reports",{}),("fs.list",{"path":"/"}),
            ("fs.mounts",{}),("screenshot",{})]
    ok=0; fail=0; lines=[]
    for cmd,p in checks:
        try:
            reply,blob=await asyncio.wait_for(c.call(cmd,**p),timeout=20)
            detail=f"{len(blob)}B binary" if blob else str(list(reply.keys()))
            if cmd=="screenshot" and blob:
                open("../live_screenshot.jpg","wb").write(blob)
                detail+=" -> saved live_screenshot.jpg"
            lines.append(f"  PASS  {cmd:<14} {detail}"); ok+=1
        except Exception as e:
            lines.append(f"  FAIL  {cmd:<14} {type(e).__name__}: {e}"); fail+=1
    try:
        await c.call("input",buttons=[],hold_ms=30,sticks={"lx":0,"ly":0,"rx":0,"ry":0})
        lines.append("  PASS  input          neutral input accepted"); ok+=1
    except Exception as e:
        lines.append(f"  FAIL  input          {e}"); fail+=1
    await c.aclose()
    print("\n".join(lines))
    print(f"\nRESULT: {ok} passed, {fail} failed")
    return fail

async def main():
    print(f"watching {HOST}:6060 (token {TOKEN[:8]}...)")
    while time.monotonic()<DEADLINE:
        if await port_open():
            print("agent port OPEN — running verification\n")
            await asyncio.sleep(2)  # let it settle
            fails=await verify()
            return 0 if fails==0 else 1
        await asyncio.sleep(4)
    print("timed out: agent never came online (console not rebooted yet?)")
    return 2

sys.exit(asyncio.run(main()))
