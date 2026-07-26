"""Raw i2c and GPIO. Gated by allow_hardware in the agent config, and
able to damage hardware — deep tier, off by default."""

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


# --- low-level hardware buses (gated by allow_hardware in config) -------------


@tool("hardware")
async def i2c_read(device: int = 2, length: int = 1, reg: int | None = None) -> str:
    """Read from an i2c device (default 2 = Tmp451 temp sensor). Optionally
    write `reg` first (register read). Returns hex. Needs allow_hardware=true."""
    kwargs = {"device": device, "len": length}
    if reg is not None:
        kwargs["reg"] = reg
    reply, blob = await client().call("i2c.read", **kwargs)
    return json.dumps({"device": reply.get("device"), "hex": blob.hex()})


@tool("hardware")
async def i2c_write(device: int, hex_bytes: str, confirm: str | None = None) -> str:
    """Write raw bytes (hex string) to an i2c device. DANGEROUS — writes to the
    PMIC and similar can physically damage the console and are not undoable.
    Needs allow_hardware=true. Call once without `confirm` first."""
    require_confirmation(
        "i2c_write",
        f"write 0x{hex_bytes.replace(' ', '')} to i2c device {device}; on the PMIC "
        f"this can permanently damage hardware",
        confirm,
    )
    data = bytes.fromhex(hex_bytes.replace(" ", ""))
    reply, _ = await client().call("i2c.write", payload=data, device=device)
    return f"Wrote {reply.get('written')} bytes to i2c device {device}"


@tool("hardware")
async def gpio_read(pad: int = 25) -> str:
    """Read a GPIO pad value (default 25 = Vol+ button). Needs allow_hardware=true."""
    reply, _ = await client().call("gpio.read", pad=pad)
    return json.dumps({"pad": reply.get("pad"), "value": reply.get("value"),
                       "high": reply.get("high")})
