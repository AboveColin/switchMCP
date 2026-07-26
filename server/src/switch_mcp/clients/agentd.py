"""Native client for the switch-agentd wire protocol.

Length-prefixed JSON frames; binary payloads follow the JSON header as raw
bytes. The agent side is agent/source/server.cpp.
"""

from __future__ import annotations

import asyncio
import hashlib
import hmac
import json
import os
import struct
from typing import Any

from .base import UNREACHABLE_GUIDANCE, ClientError, SwitchClient

CHUNK = 1 << 20  # 1 MiB, matches agent-side cap
MAX_JSON = 64 * 1024

# Generous: this console's Wi-Fi enters power-save when idle, and the first
# SYN after a quiet period can take several seconds to get through (measured
# 30%+ packet loss and 60-80ms RTT while waking). 5s produced spurious
# "unreachable" failures against a perfectly healthy console.
CONNECT_TIMEOUT = 15.0

# Per-command deadline. Everything holds the connection lock while it runs, so
# an un-bounded read doesn't just hang one tool, it freezes every tool. These
# are wall-clock budgets for a single round-trip, not for a whole transfer.
DEFAULT_TIMEOUT = 20.0
COMMAND_TIMEOUTS: dict[str, float] = {
    # Bulk data: a 1 MiB chunk over Wi-Fi, plus device-side seek/read.
    "fs.read": 60.0,
    "fs.write": 60.0,
    "nand.read": 90.0,
    "album.download": 60.0,
    # Device-side work that is legitimately slow.
    "screenshot": 30.0,
    "titles": 60.0,        # NsApplicationControlData for every installed title
    "debug.search": 120.0,  # scans process memory on-device
    "save.backup": 300.0,
    "save.restore": 300.0,
    # Long-polls: these block on the device by design. Must exceed the agent's
    # own 20s cap or the client gives up on a command that is about to answer.
    "wait_event": 30.0,
    "record_input": 30.0,
    # Scans walk mapped memory a chunk at a time, one IPC per chunk.
    "search.begin": 180.0,
    "search.next": 120.0,
    # Fire-and-forget: the console stops responding *because* it worked.
    "reboot": 5.0,
    "shutdown": 5.0,
    "reboot_to_payload": 5.0,
    "agent.restart": 5.0,
}

# Commands safe to re-send when the transport dies mid-flight. Everything
# absent from this set is assumed to have side effects: if the agent applied it
# before the connection dropped, a blind retry applies it twice.
# Commands whose success looks exactly like a dropped connection: the console
# stops answering *because* the command worked. The agent defers the actual
# transition ~500ms so the ack usually arrives, but on a slow link it may not —
# so losing the connection right after sending these is treated as success.
FIRE_AND_FORGET = frozenset({"reboot", "shutdown", "reboot_to_payload", "agent.restart"})

IDEMPOTENT_COMMANDS = frozenset({
    "hello", "agent.info", "sysinfo", "ps", "titles", "title_icon", "running_app",
    "crash_reports", "read_log", "screenshot", "console_info", "controllers",
    "get_clocks", "get_brightness", "get_volume", "get_time",
    "fs.list", "fs.stat", "fs.mounts", "fs.read", "nand.read",
    "album.list", "album.download",
    "debug.memmap", "debug.threads", "debug.registers", "debug.read_mem",
    "debug.search", "cheat.status", "cheat.list",
    "i2c.read", "gpio.read",
    # Breakpoint slots are set-to-a-value, not incremented, so re-applying the
    # same call lands on the same state. Worth having here: a dropped link
    # during cleanup would otherwise leave scarce hardware slots armed.
    "debug.set_watchpoint", "debug.set_breakpoint", "debug.clear_breakpoint",
    "fs.hash",
})


class AgentClient(SwitchClient):
    """Single persistent connection to switch-agentd; reconnects on drop."""

    def __init__(self, host: str, port: int = 6060, token: str = ""):
        self.host = host
        self.port = port
        self.token = token
        self._reader: asyncio.StreamReader | None = None
        self._writer: asyncio.StreamWriter | None = None
        self._id = 0
        self._lock = asyncio.Lock()
        # Populated from the hello reply: what this agent build actually
        # permits, so callers can check before issuing a doomed command.
        self.capabilities: dict[str, Any] = {}
        self.auth_method: str | None = None

    @classmethod
    def from_env(cls) -> "AgentClient":
        host = os.environ.get("SWITCH_HOST")
        if not host:
            raise RuntimeError("SWITCH_HOST env var not set (console IP)")
        return cls(
            host,
            int(os.environ.get("SWITCH_PORT", "6060")),
            os.environ.get("SWITCH_TOKEN", ""),
        )

    async def _connect(self) -> None:
        self._reader, self._writer = await asyncio.wait_for(
            asyncio.open_connection(self.host, self.port), timeout=CONNECT_TIMEOUT
        )
        try:
            reply = await self._authenticate()
        except (ClientError, asyncio.IncompleteReadError, ConnectionError, OSError):
            # An old agent rejects the probe and hangs up; retry the legacy way
            # before concluding the credentials are wrong.
            if self.auth_method is not None:
                raise
            reply = await self._authenticate_legacy()
        if not reply.get("ok"):
            raise ClientError("auth", "agent rejected handshake")

    async def _authenticate(self) -> dict[str, Any]:
        """Prove knowledge of the token without ever sending it.

        Asks for challenge-response; the agent replies with a random nonce and
        we return HMAC-SHA256(token, nonce). A passive observer learns nothing
        reusable, because that nonce is never offered again.

        Agents older than 0.2.0 do not understand `auth: "hmac"`. They see a
        hello with no token, reject it and CLOSE the connection — so the
        fallback needs a fresh socket, not a retry on the dead one. That costs
        one extra connect against old agents and nothing against current ones,
        which is the right trade: sending the token unconditionally "just in
        case" would defeat the entire point for the agents that do support this.
        """
        reply, _ = await self._roundtrip(
            {"cmd": "hello", "auth": "hmac", "version": 1}, timeout=CONNECT_TIMEOUT
        )
        challenge = reply.get("challenge")
        if challenge:
            mac = hmac.new(
                self.token.encode(), challenge.encode(), hashlib.sha256
            ).hexdigest()
            reply, _ = await self._roundtrip(
                {"cmd": "auth", "hmac": mac}, timeout=CONNECT_TIMEOUT
            )
            self.auth_method = "hmac"
            self._store_capabilities(reply)
            return reply

        # No challenge: this agent predates challenge-response.
        return await self._authenticate_legacy()

    async def _authenticate_legacy(self) -> dict[str, Any]:
        """Pre-0.2.0 handshake: the token goes over the wire in cleartext."""
        await self._discard_connection()
        self._reader, self._writer = await asyncio.wait_for(
            asyncio.open_connection(self.host, self.port), timeout=CONNECT_TIMEOUT
        )
        reply, _ = await self._roundtrip(
            {"cmd": "hello", "token": self.token, "version": 1},
            timeout=CONNECT_TIMEOUT,
        )
        self.auth_method = "legacy"
        self._store_capabilities(reply)
        return reply

    def _store_capabilities(self, reply: dict[str, Any]) -> None:
        self.capabilities = {
            k: reply[k]
            for k in ("agent_version", "protocol_version", "fw", "tier", "emummc",
                      "allow_nand_write", "allow_overclock", "allow_hardware",
                      "commands")
            if k in reply
        }

    def supports(self, cmd: str) -> bool:
        """Whether the connected agent advertises `cmd` at its current tier.

        Agents older than 0.2.0 send no command list; assume support rather than
        blocking commands that would in fact work.
        """
        cmds = self.capabilities.get("commands")
        return True if cmds is None else cmd in cmds

    async def _roundtrip(
        self,
        msg: dict[str, Any],
        payload: bytes = b"",
        timeout: float = DEFAULT_TIMEOUT,
        sent_flag: list[bool] | None = None,
    ) -> tuple[dict[str, Any], bytes]:
        """One request/response exchange under a wall-clock deadline.

        `sent_flag`, if given, is set to True once the request has actually
        reached the socket — after that point a failure may mean the agent
        already applied the command, so the caller must not blindly retry.
        """
        assert self._reader and self._writer
        self._id += 1
        msg["id"] = self._id
        if payload:
            msg["bin"] = len(payload)
        raw = json.dumps(msg).encode()
        if len(raw) > MAX_JSON:
            raise ClientError("frame_too_big", "request JSON exceeds 64 KiB")

        async def exchange() -> tuple[dict[str, Any], bytes]:
            assert self._reader and self._writer
            self._writer.write(struct.pack("<I", len(raw)) + raw)
            if payload:
                self._writer.write(struct.pack("<Q", len(payload)) + payload)
            await self._writer.drain()
            if sent_flag is not None:
                sent_flag[0] = True

            (jlen,) = struct.unpack("<I", await self._reader.readexactly(4))
            reply = json.loads(await self._reader.readexactly(jlen))
            blob = b""
            if "bin" in reply:
                (blen,) = struct.unpack("<Q", await self._reader.readexactly(8))
                blob = await self._reader.readexactly(blen)
            return reply, blob

        reply, blob = await asyncio.wait_for(exchange(), timeout=timeout)
        if "error" in reply:
            raise ClientError(reply["error"]["code"], reply["error"]["message"])
        return reply, blob

    async def call(
        self, cmd: str, payload: bytes = b"", **params: Any
    ) -> tuple[dict[str, Any], bytes]:
        transient = (ConnectionError, asyncio.IncompleteReadError, asyncio.TimeoutError,
                     OSError)
        timeout = COMMAND_TIMEOUTS.get(cmd, DEFAULT_TIMEOUT)
        idempotent = cmd in IDEMPOTENT_COMMANDS

        async with self._lock:
            # Try once on the existing connection (if any), then reconnect and
            # retry — but only when retrying is actually safe. If the request
            # already reached the agent, a non-idempotent command may have been
            # applied, and re-sending it would apply it twice (a second reboot,
            # a duplicated fs.write, a repeated button press).
            for attempt in (0, 1):
                sent = [False]
                try:
                    if self._writer is None or self._writer.is_closing():
                        await self._connect()
                    return await self._roundtrip(
                        {"cmd": cmd, **params}, payload, timeout=timeout, sent_flag=sent
                    )
                except transient as exc:
                    await self._discard_connection()
                    if attempt == 1 and not sent[0]:
                        # Never got the request out on either attempt: this is a
                        # reachability problem, not a command problem.
                        raise ClientError(
                            "unreachable",
                            f"{type(exc).__name__} talking to {self.host}:{self.port}. "
                            f"{UNREACHABLE_GUIDANCE}",
                        ) from exc
                    if sent[0] and cmd in FIRE_AND_FORGET:
                        # Losing the console here is the expected outcome.
                        return {"ok": True, "note": "no reply: console went down as expected"}, b""
                    if attempt == 1:
                        raise
                    if sent[0] and not idempotent:
                        raise ClientError(
                            "uncertain",
                            f"'{cmd}' was sent but the connection dropped before the "
                            f"agent replied ({type(exc).__name__}). Not retrying: it "
                            f"may already have taken effect. Check device state, then "
                            f"re-issue if needed.",
                        ) from exc

    async def _discard_connection(self) -> None:
        """Drop a connection we no longer trust, without raising."""
        writer, self._writer, self._reader = self._writer, None, None
        if writer is not None and not writer.is_closing():
            writer.close()
            try:
                await writer.wait_closed()
            except (ConnectionError, OSError, asyncio.TimeoutError):
                pass

    async def aclose(self) -> None:
        await self._discard_connection()

    async def read_file(self, path: str) -> bytes:
        parts: list[bytes] = []
        offset = 0
        while True:
            reply, blob = await self.call("fs.read", path=path, offset=offset, len=CHUNK)
            parts.append(blob)
            offset += len(blob)
            if reply.get("eof"):
                return b"".join(parts)

    async def write_file(self, path: str, data: bytes) -> int:
        offset = 0
        while offset < len(data) or offset == 0:
            chunk = data[offset : offset + CHUNK]
            await self.call("fs.write", payload=chunk, path=path, offset=offset)
            offset += len(chunk)
            if not chunk:
                break
        return len(data)
