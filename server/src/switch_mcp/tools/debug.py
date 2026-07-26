"""Live process debugger: attach, memory, registers, breakpoints,
modules and backtraces.

Deep tier — off by default, enable with SWITCH_MCP_TOOLS."""

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


# --- live debugger (deep tier: process memory & registers) --------------------
#
# Model: debug_attach pauses the target; reads/writes act on the frozen process;
# debug_detach resumes it. Use for developing/RE'ing your own homebrew: dump the
# memory map, read/patch memory, inspect registers, search for values.


def _addr(a) -> int:
    """Accept an address as int or '0x...'/decimal string."""
    if isinstance(a, int):
        return a
    return int(a, 16) if str(a).lower().startswith("0x") else int(a)


@tool("debug")
async def debug_attach(pid: int = 0, pause: bool = True) -> str:
    """Attach the debugger to a process (default: foreground app). pause=True
    freezes the target (best for inspection); pause=False keeps it running while
    attached (for live memory/cheat work). Returns pid and main module base."""
    reply, _ = await client().call("debug.attach", pid=pid, pause=pause)
    reply.pop("id", None)
    return json.dumps(reply, indent=2)


@tool("debug")
async def debug_poll_events(resume: bool = True) -> str:
    """Drain queued debug events from the attached process — exceptions/faults,
    thread & process create/exit, user breaks. With a non-pausing attach, poll
    this repeatedly for a live fault/log stream. resume=True keeps it running."""
    reply, _ = await client().call("debug.poll_events", resume=resume)
    return json.dumps(reply.get("events", []), indent=2)


@tool("debug")
async def debug_continue() -> str:
    """Resume a paused attached target without detaching."""
    await client().call("debug.continue")
    return "Target resumed (still attached)"


@tool("debug")
async def debug_break() -> str:
    """Break into (pause) a running attached target."""
    await client().call("debug.break")
    return "Target paused"


@tool("debug")
async def debug_detach() -> str:
    """Detach the debugger and RESUME the target process."""
    await client().call("debug.detach")
    return "Detached; target resumed"


@tool("debug")
async def debug_read_mem(addr: str, length: int = 256) -> str:
    """Read `length` bytes (max 1 MiB) from the attached process at `addr`
    (int or '0x...'). Returns hex. Requires debug_attach first."""
    reply, blob = await client().call("debug.read_mem", addr=_addr(addr), len=length)
    return json.dumps({"addr": hex(reply.get("addr", 0)), "hex": blob.hex()})


@tool("debug")
async def debug_write_mem(addr: str, hex_bytes: str) -> str:
    """Write raw bytes (given as a hex string, e.g. '1f2003d5') to the attached
    process at `addr`. Patches live memory. Requires debug_attach first."""
    data = bytes.fromhex(hex_bytes.replace(" ", ""))
    reply, _ = await client().call("debug.write_mem", payload=data, addr=_addr(addr))
    return f"Wrote {reply.get('written')} bytes at {addr}"


@tool("debug")
async def debug_set_watchpoint(
    addr: str, size: int = 4, mode: str = "write", slot: int = 0
) -> str:
    """Break when a memory address is accessed — the fastest way to find what
    writes a value.

    Instead of repeatedly reading memory and diffing, set a watchpoint on the
    address and let the hardware catch the exact instruction that touches it;
    the hit arrives via debug_poll_events with the faulting PC. This is how you
    go from "the health value lives at 0x… " to "this code decrements it".

    mode: "write" (default), "read", or "rw". size: 1, 2, 4 or 8 bytes, and the
    range may not cross an 8-byte boundary. slot: hardware register index —
    only about 4 watchpoint slots exist, so free them with
    debug_clear_breakpoint when done. Requires debug_attach first."""
    reply, _ = await client().call(
        "debug.set_watchpoint", addr=_addr(addr), size=size, mode=mode, slot=slot
    )
    reply.pop("id", None)
    return json.dumps(reply, indent=2)


@tool("debug")
async def debug_set_breakpoint(addr: str, slot: int = 0) -> str:
    """Break when an instruction address is executed (hardware breakpoint).

    `addr` must be 4-byte aligned. Hits arrive via debug_poll_events. Only about
    6 breakpoint slots exist in hardware; free them with debug_clear_breakpoint.
    Requires debug_attach first."""
    reply, _ = await client().call("debug.set_breakpoint", addr=_addr(addr), slot=slot)
    reply.pop("id", None)
    return json.dumps(reply, indent=2)


@tool("debug")
async def debug_clear_breakpoint(slot: int = 0, watchpoint: bool = False) -> str:
    """Free a hardware breakpoint or watchpoint slot.

    Slots are a scarce hardware resource and stay armed until cleared, so a
    forgotten slot silently costs you the next one. Set watchpoint=True to clear
    a data watchpoint rather than an instruction breakpoint."""
    reply, _ = await client().call(
        "debug.clear_breakpoint", slot=slot, watchpoint=watchpoint
    )
    reply.pop("id", None)
    return json.dumps(reply, indent=2)


@tool("debug")
async def debug_write_registers(registers: dict, thread_id: int = 0) -> str:
    """Write CPU registers of a stopped thread.

    `registers` names only what should change, e.g. {"pc": "0x8001234"} or
    {"x0": 1}; everything else keeps its current value. Use to redirect
    execution, skip a faulting instruction, or force a return value. Accepts
    ints or "0x..." strings. thread_id 0 = first thread.

    Requires debug_attach first, and the invasive tier."""
    coerced = {k: _addr(v) for k, v in registers.items()}
    reply, _ = await client().call(
        "debug.write_registers", registers=coerced, thread_id=thread_id
    )
    reply.pop("id", None)
    return json.dumps(reply, indent=2)


@tool("debug")
async def debug_modules(pid: int = 0) -> str:
    """List a process's loaded modules with base address, size and build ID.

    This is what turns raw addresses into something you can act on: subtract a
    module base from an address to get an RVA like `main+0x1a2f4`, which is the
    form Ghidra and IDA use. The build ID identifies that exact binary, so it is
    the key for matching a symbol map or an existing RE database to what is
    running.

    Works without attaching (uses ldr:dmnt). pid 0 = the attached process, or
    the foreground app if nothing is attached. Call this before debug_read_mem
    or debug_backtrace so the addresses mean something."""
    reply, _ = await client().call("debug.modules", pid=pid)
    reply.pop("id", None)
    return json.dumps(reply, indent=2)


@tool("debug")
async def debug_backtrace(thread_id: int = 0, max_frames: int = 32) -> str:
    """Walk a stopped thread's frame-pointer chain into a call stack.

    Turns "it faulted somewhere" into the path it took to get there. Requires
    debug_attach first (the thread must be stopped). thread_id 0 = first thread.

    Addresses are absolute — pair with debug_modules and subtract the module
    base to get RVAs. The walk stops on its own when the chain stops looking
    like real frames, so a short backtrace usually means the frame pointer was
    optimised away rather than that something failed."""
    reply, _ = await client().call(
        "debug.backtrace", thread_id=thread_id, max_frames=max_frames
    )
    reply.pop("id", None)
    return json.dumps(reply, indent=2)


@tool("debug")
async def debug_memmap() -> str:
    """Dump the attached process's memory map (mapped regions with addr/size/
    type/permissions). Requires debug_attach first."""
    reply, _ = await client().call("debug.memmap")
    return json.dumps(reply.get("regions", []), indent=2)


@tool("debug")
async def debug_threads() -> str:
    """List thread IDs of the attached process. Requires debug_attach first."""
    reply, _ = await client().call("debug.threads")
    return json.dumps(reply.get("threads", []))


@tool("debug")
async def debug_registers(thread_id: int = 0) -> str:
    """Read a thread's CPU registers (x0–x28, fp, lr, sp, pc). thread_id 0 =
    first thread. Requires debug_attach first."""
    reply, _ = await client().call("debug.registers", thread_id=thread_id)
    reply.pop("id", None)
    return json.dumps(reply, indent=2)


@tool("debug")
async def debug_search(value: int, width: int = 4, max_results: int = 64) -> str:
    """Search the attached process's writable memory for a little-endian integer
    `value` of `width` bytes (1/2/4/8). Returns matching addresses — the basis
    of cheat/mod development. Requires debug_attach first."""
    reply, _ = await client().call("debug.search", value=value, width=width, max=max_results)
    return json.dumps(
        {"count": reply.get("count"), "truncated": reply.get("truncated"),
         "matches": reply.get("matches", [])}, indent=2)
