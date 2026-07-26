"""Find a switch-agentd on the local network.

The Switch doesn't advertise mDNS, so we sweep the host's /24 by attempting the
agent handshake on the configured port. Whatever answers `hello` with the right
token is the console.
"""

from __future__ import annotations

import asyncio
import ipaddress
import json
import socket
import struct


def _local_ipv4() -> str | None:
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))  # no packets sent; just picks the egress iface
        return s.getsockname()[0]
    except OSError:
        return None
    finally:
        s.close()


async def _probe(host: str, port: int, token: str, timeout: float) -> dict | None:
    try:
        reader, writer = await asyncio.wait_for(
            asyncio.open_connection(host, port), timeout=timeout
        )
    except (OSError, asyncio.TimeoutError):
        return None
    try:
        msg = json.dumps({"id": 0, "cmd": "hello", "token": token, "version": 1}).encode()
        writer.write(struct.pack("<I", len(msg)) + msg)
        await writer.drain()
        (jlen,) = struct.unpack("<I", await asyncio.wait_for(reader.readexactly(4), timeout))
        reply = json.loads(await reader.readexactly(jlen))
        if reply.get("ok"):
            return {"host": host, "fw": reply.get("fw"), "agent_version": reply.get("agent_version")}
    except (OSError, asyncio.IncompleteReadError, asyncio.TimeoutError, ValueError):
        return None
    finally:
        writer.close()
    return None


async def scan(port: int = 6060, token: str = "", timeout: float = 0.4) -> list[dict]:
    """Probe every host on the local /24 and return those running the agent."""
    local = _local_ipv4()
    if not local:
        return []
    net = ipaddress.ip_network(f"{local}/24", strict=False)
    sem = asyncio.Semaphore(64)

    async def guarded(ip: str):
        async with sem:
            return await _probe(ip, port, token, timeout)

    results = await asyncio.gather(*(guarded(str(ip)) for ip in net.hosts()))
    return [r for r in results if r]
