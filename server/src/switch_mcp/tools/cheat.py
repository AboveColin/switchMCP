"""Cheat engine and live (unpaused) memory access, including the
narrowing value search.

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


# --- cheat engine (dmnt:cht) --------------------------------------------------


@tool("cheat")
async def live_meta() -> str:
    """Title ID, main-module base/size, heap extents and build ID of the running
    game — WITHOUT attaching a debugger and without pausing it.

    Start here for any memory work on a live game: it gives the regions
    find_value should search and the build ID that identifies the binary."""
    reply, _ = await client().call("cheat.meta")
    reply.pop("id", None)
    return json.dumps(reply, indent=2)


@tool("cheat")
async def live_read_mem(addr: str, length: int = 256) -> str:
    """Read memory of the RUNNING game without pausing it.

    This is the important difference from debug_read_mem: attaching a debugger
    freezes the target, so anything that only exists while the game is moving —
    a draining health bar, a countdown — cannot be observed that way. Returns
    hex. Requires Atmosphère's cheat module."""
    reply, blob = await client().call("cheat.read_mem", addr=_addr(addr), len=length)
    return json.dumps({"addr": hex(reply.get("addr", 0)), "hex": blob.hex()})


@tool("cheat")
async def live_write_mem(addr: str, hex_bytes: str) -> str:
    """Write memory of the RUNNING game without pausing it. Invasive tier.

    Patches a live process — the effect is immediate and there is no undo beyond
    writing the old bytes back, so read the region first."""
    data = bytes.fromhex(hex_bytes.replace(" ", ""))
    reply, _ = await client().call("cheat.write_mem", payload=data, addr=_addr(addr))
    return f"Wrote {reply.get('written')} bytes at {addr}"


@tool("cheat")
async def live_mappings(limit: int = 60) -> str:
    """List the running game's actual memory mappings (address, size, perms, type).

    Worth knowing why this matters: the heap *extent* reported by live_meta is a
    reserved ~8 GB address range that is almost entirely unmapped. Only these
    mappings are real. find_value walks them automatically, but this is how you
    see what there is to search and pick a region deliberately."""
    reply, _ = await client().call("cheat.mappings", limit=limit)
    reply.pop("id", None)
    return json.dumps(reply, indent=2)


@tool("cheat")
async def find_value(value: int, width: int = 4, region: str = "",
                     max_bytes: int = 536870912) -> str:
    """Start a narrowing memory search for `value` in the running game.

    This is the first step of the standard value hunt. A single scan is not
    enough on its own — searching for 100 will match tens of thousands of
    addresses. Follow with `narrow_search` after changing the value in-game:

        find_value(100)                  -> 40k candidates
        (take damage in game)
        narrow_search(op="decreased")    -> 300 candidates
        narrow_search(op="eq", value=87) -> 2 candidates

    Candidates are kept ON the console between calls, so each step costs one
    round-trip instead of shipping addresses back and forth.

    Only real mappings are scanned — the reserved heap extent is mostly
    unmapped, so a naive sweep would be thousands of failed reads. region=""
    (default) covers everything readable+writable; "heap" or "main" narrow it.

    width: 1, 2, 4 or 8 bytes. max_bytes bounds the scan so a large game cannot
    tie up the console for minutes. The reply says explicitly when the candidate
    cap or the byte budget was hit, rather than looking complete."""
    reply, _ = await client().call(
        "search.begin", value=value, width=width, region=region, max_bytes=max_bytes
    )
    reply.pop("id", None)
    return json.dumps(reply, indent=2)


@tool("cheat")
async def narrow_search(op: str = "eq", value: int = 0) -> str:
    """Filter the surviving candidates from find_value against a new observation.

    Operators needing a value: eq, ne, gt, lt.
    Operators comparing against the previous scan: changed, unchanged,
    increased, decreased — these are what make the search converge, because you
    rarely know the exact new number but always know which way it moved.

    Repeat until a handful of addresses remain, then read them with
    live_read_mem to confirm."""
    reply, _ = await client().call("search.next", op=op, value=value)
    reply.pop("id", None)
    return json.dumps(reply, indent=2)


@tool("cheat")
async def search_results(limit: int = 50) -> str:
    """List the surviving candidate addresses and their current values."""
    reply, _ = await client().call("search.results", limit=limit)
    reply.pop("id", None)
    return json.dumps(reply, indent=2)


@tool("cheat")
async def search_reset() -> str:
    """Discard the candidate set and free the memory it holds on the console."""
    await client().call("search.reset")
    return "Search reset"


@tool("cheat")
async def cheat_status() -> str:
    """Whether a cheat process is attached, plus cheat and frozen-address counts.
    Requires the dmnt cheat module enabled on the console."""
    reply, _ = await client().call("cheat.status")
    reply.pop("id", None)
    return json.dumps(reply, indent=2)


@tool("cheat")
async def cheat_list() -> str:
    """List loaded cheats (id, name, enabled, opcode count)."""
    reply, _ = await client().call("cheat.list")
    return json.dumps(reply.get("cheats", []), indent=2)


@tool("cheat")
async def cheat_toggle(cheat_id: int) -> str:
    """Enable/disable a cheat by id (see cheat_list)."""
    await client().call("cheat.toggle", cheat_id=cheat_id)
    return f"Toggled cheat {cheat_id}"


@tool("cheat")
async def freeze_address(addr: str, width: int = 4) -> str:
    """Freeze a memory address at its current value (classic cheat primitive).
    width in bytes (1/2/4/8). Returns the frozen value."""
    reply, _ = await client().call("cheat.freeze", addr=_addr(addr), width=width)
    return f"Frozen {addr} at value {reply.get('frozen_value')}"


@tool("cheat")
async def unfreeze_address(addr: str) -> str:
    """Stop freezing a previously frozen address."""
    await client().call("cheat.unfreeze", addr=_addr(addr))
    return f"Unfroze {addr}"
