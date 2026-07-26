#!/usr/bin/env python3
"""Deploy the built agent to the console, verified, then reboot to load it.

Wi-Fi on this console drops the link often enough that a single-shot upload is
not reliable: one attempt here silently truncated the .nsp by 166 bytes, which
a reboot would have turned into an agent that never comes back. So every step
retries and the reboot only happens once the on-device hash matches.
"""
import asyncio, hashlib, os, pathlib, sys, time

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "server" / "src"))
from switch_mcp.clients.agentd import AgentClient  # noqa: E402

REMOTE = "/atmosphere/contents/420000000000AE57/exefs.nsp"
LOCAL = ROOT / "agent" / "switch-agentd.nsp"


async def with_retry(fn, what, tries=40, delay=6):
    for i in range(1, tries + 1):
        try:
            return await fn()
        except Exception as e:
            print(f"  {what}: attempt {i} {type(e).__name__}")
            await asyncio.sleep(delay)
    raise SystemExit(f"gave up on {what}")


async def main():
    data = LOCAL.read_bytes()
    want = hashlib.sha256(data).hexdigest()
    print(f"local build: {len(data)} bytes sha256={want[:16]}")

    async def upload_and_verify():
        c = AgentClient.from_env()
        try:
            try:
                h, _ = await c.call("fs.hash", path=REMOTE)
                if h["sha256"] == want:
                    print("  already up to date on device")
                    return True
            except Exception:
                pass
            await c.write_file(REMOTE, data)
            h, _ = await c.call("fs.hash", path=REMOTE)
            if h["sha256"] != want:
                raise RuntimeError(f"hash mismatch after upload ({h['size']} bytes)")
            print(f"  uploaded and verified ({len(data)} bytes)")
            return True
        finally:
            await c.aclose()

    await with_retry(upload_and_verify, "upload")

    if "--no-reboot" in sys.argv:
        print("image verified; skipping reboot (--no-reboot)")
        return

    async def restart():
        c = AgentClient.from_env()
        try:
            r, _ = await c.call("agent.restart")
            print("  restart:", r.get("note"))
            return True
        finally:
            await c.aclose()

    await with_retry(restart, "restart", tries=10)

    async def wait_back():
        c = AgentClient.from_env()
        try:
            i, _ = await c.call("agent.info")
            return c, i
        except Exception:
            await c.aclose()
            raise

    # Poll hard rather than politely: the agent clears the lockscreen itself
    # now, but if that is disabled the fallback press below is racing the
    # lockscreen's own sleep timer, and 6-second retries lose that race.
    await asyncio.sleep(8)
    c, info = await with_retry(wait_back, "reconnect", tries=120, delay=1)
    print(f"agent {info['agent_version']} up; tier={c.capabilities.get('tier')} "
          f"emummc={c.capabilities.get('emummc')} cmds={len(c.capabilities.get('commands', []))}")

    # Clear the lockscreen immediately. The console boots to it, and the
    # lockscreen auto-sleeps even with auto-sleep set to Never in Settings —
    # once it does the network stack goes down and recovery needs a physical
    # power-cycle. The window is short, so this runs before anything else.
    #
    # B, not A. The lockscreen unlocks on any button, but if the console is
    # already past it we are sending input to the home menu, where A launches
    # whatever happens to be selected — doing that here opened the Nintendo
    # Switch Online app. B is "back/cancel" everywhere, so it unlocks the
    # lockscreen and does nothing anywhere else.
    # Belt and braces. The agent does this itself at boot
    # (clear_lockscreen_on_boot), which is the reliable path since it has no
    # network to wait for; this only helps if that is turned off.
    try:
        for _ in range(2):
            await c.call("input", buttons=["B"], hold_ms=120,
                         sticks={"lx": 0, "ly": 0, "rx": 0, "ry": 0})
            await asyncio.sleep(0.3)
        print("  sent a backup B press (agent clears the lockscreen itself)")
    except Exception:
        print("  WARNING: could not clear the lockscreen; press a button on the "
              "console or it will sleep and drop off the network")
    lg, _ = await c.call("read_log", max_bytes=2000)
    lines = lg["content"].splitlines()
    boot = max((i for i, l in enumerate(lines) if "config loaded" in l), default=0)
    for l in lines[boot:boot + 4]:
        print("  ", l)
    await c.aclose()


asyncio.run(main())
