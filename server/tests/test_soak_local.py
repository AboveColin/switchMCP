"""Client-side soak: sustained round-trips against the in-process fake agent to
catch leaks/wedges in OUR code (the half of Phase 9's soak that doesn't need a
console). Device-side soak lives in soak.py and requires hardware."""

from __future__ import annotations

import asyncio
import gc
import tracemalloc

import pytest

from switch_mcp.clients.agentd import AgentClient
from test_protocol import FakeAgent


@pytest.mark.asyncio
async def test_no_client_leak_under_load():
    data = bytes(range(256)) * 4096  # ~1 MiB file for chunked reads
    a = FakeAgent(file_data=data)
    await a.start()
    c = AgentClient("127.0.0.1", a.port, "secret")

    # Warm up, then measure allocation growth across many mixed operations.
    for _ in range(200):
        await c.call("sysinfo")
        await c.screenshot()
    gc.collect()
    tracemalloc.start()
    base = tracemalloc.take_snapshot()

    for _ in range(3000):
        await c.call("sysinfo")
        await c.screenshot()
        await c.read_file("/f.bin")
    gc.collect()
    after = tracemalloc.take_snapshot()
    tracemalloc.stop()

    grew = sum(s.size_diff for s in after.compare_to(base, "filename"))
    await c.aclose()
    await a.stop()

    # A steady-state client shouldn't retain material memory across 3k ops.
    # Allow generous slack for allocator/interpreter noise; a real leak would
    # scale with iteration count and blow far past this.
    assert grew < 512 * 1024, f"client retained {grew} bytes across 3000 ops"
