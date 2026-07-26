"""Tests for the agentd client against an in-process fake agent that speaks the
documented wire format."""

from __future__ import annotations

import asyncio
import hashlib
import hmac
import json
import os
import struct

import pytest

from switch_mcp.clients.agentd import AgentClient
from switch_mcp.clients.base import ClientError


class FakeAgent:
    """Minimal protocol server for tests. Serves file bytes in 1 MiB chunks,
    echoes writes, and can be told to drop the first connection."""

    def __init__(self, token="secret", file_data=b"", drop_first=False,
                 die_on=None, stall_on=None, tier="control", emummc=False,
                 commands=None):
        self.token = token
        self.file_data = file_data
        self.written = bytearray()
        self.drop_first = drop_first
        # Command name that the agent "applies" and then dies without replying,
        # simulating a link drop between effect and acknowledgement.
        self.die_on = die_on
        # Command name the agent accepts but never answers, to exercise the
        # client's per-command deadline.
        self.stall_on = stall_on
        self.applied: list[str] = []
        self.tier = tier
        self.emummc = emummc
        self.commands = commands if commands is not None else [
            "agent.info", "sysinfo", "screenshot", "fs.read", "fs.write", "reboot",
        ]
        self._connections = 0
        self._nonce = ""
        self.server = None
        self.port = None

    async def start(self):
        self.server = await asyncio.start_server(self._handle, "127.0.0.1", 0)
        self.port = self.server.sockets[0].getsockname()[1]

    async def stop(self):
        self.server.close()
        # On Python 3.13+ wait_closed() blocks until active connections finish;
        # a persistent test client may still be attached, so guard with a
        # timeout instead of hanging.
        try:
            await asyncio.wait_for(self.server.wait_closed(), timeout=2)
        except asyncio.TimeoutError:
            pass

    async def _read_frame(self, reader):
        (jlen,) = struct.unpack("<I", await reader.readexactly(4))
        msg = json.loads(await reader.readexactly(jlen))
        payload = b""
        if "bin" in msg:
            (blen,) = struct.unpack("<Q", await reader.readexactly(8))
            payload = await reader.readexactly(blen)
        return msg, payload

    async def _write_frame(self, writer, msg, payload=b""):
        if payload:
            msg["bin"] = len(payload)
        raw = json.dumps(msg).encode()
        writer.write(struct.pack("<I", len(raw)) + raw)
        if payload:
            writer.write(struct.pack("<Q", len(payload)) + payload)
        await writer.drain()

    def _hello_reply(self, mid):
        return {
            "id": mid, "ok": True, "fw": "18.1.0", "agent_version": "0.2.0",
            "tier": self.tier, "emummc": self.emummc,
            "commands": list(self.commands),
        }

    async def _handle(self, reader, writer):
        self._connections += 1
        if self.drop_first and self._connections == 1:
            writer.close()
            return
        try:
            while True:
                msg, payload = await self._read_frame(reader)
                mid, cmd = msg.get("id"), msg.get("cmd")
                if cmd is not None and cmd not in ("hello", "auth"):
                    self.applied.append(cmd)
                if cmd == self.die_on:
                    # Effect happened; connection dies before the reply.
                    writer.close()
                    return
                if cmd == self.stall_on:
                    await asyncio.sleep(300)
                if cmd == "hello" and msg.get("auth") == "hmac":
                    # Server-issued nonce: the token never crosses the wire.
                    self._nonce = os.urandom(32).hex()
                    await self._write_frame(writer, {
                        "id": mid, "challenge": self._nonce, "algo": "hmac-sha256",
                    })
                    continue
                elif cmd == "auth":
                    want = hmac.new(self.token.encode(), self._nonce.encode(),
                                    hashlib.sha256).hexdigest()
                    if msg.get("hmac") != want:
                        await self._write_frame(
                            writer, {"id": mid, "error": {"code": "auth",
                                                          "message": "bad mac"}}
                        )
                        break
                    await self._write_frame(writer, self._hello_reply(mid))
                elif cmd == "hello":
                    ok = msg.get("token") == self.token
                    if not ok:
                        await self._write_frame(
                            writer, {"id": mid, "error": {"code": "auth", "message": "no"}}
                        )
                        break
                    await self._write_frame(writer, self._hello_reply(mid))
                elif cmd == "fs.read":
                    off, ln = msg["offset"], msg["len"]
                    chunk = self.file_data[off : off + ln]
                    eof = off + len(chunk) >= len(self.file_data)
                    await self._write_frame(writer, {"id": mid, "eof": eof}, chunk)
                elif cmd == "fs.write":
                    self.written[msg["offset"] : msg["offset"] + len(payload)] = payload
                    await self._write_frame(writer, {"id": mid, "written": len(payload)})
                elif cmd == "screenshot":
                    await self._write_frame(writer, {"id": mid, "jpeg": True}, b"\xff\xd8JPEG")
                elif cmd == "boom":
                    await self._write_frame(
                        writer, {"id": mid, "error": {"code": "kaboom", "message": "nope"}}
                    )
                else:
                    await self._write_frame(writer, {"id": mid, "ok": True})
        except (asyncio.IncompleteReadError, ConnectionError):
            pass
        finally:
            writer.close()


@pytest.fixture
async def agent():
    a = FakeAgent()
    await a.start()
    yield a
    await a.stop()


@pytest.mark.asyncio
async def test_handshake_and_simple_call(agent):
    c = AgentClient("127.0.0.1", agent.port, "secret")
    reply, _ = await c.call("sysinfo")
    assert reply["ok"] is True


@pytest.mark.asyncio
async def test_bad_token_rejected():
    a = FakeAgent(token="right")
    await a.start()
    c = AgentClient("127.0.0.1", a.port, "wrong")
    with pytest.raises(ClientError) as e:
        await c.call("sysinfo")
    assert e.value.code == "auth"
    await a.stop()


@pytest.mark.asyncio
async def test_chunked_download_reassembles():
    data = bytes(range(256)) * (5000)  # ~1.28 MiB > one chunk
    a = FakeAgent(file_data=data)
    await a.start()
    c = AgentClient("127.0.0.1", a.port, "secret")
    got = await c.read_file("/big.bin")
    assert got == data
    await a.stop()


@pytest.mark.asyncio
async def test_upload_roundtrip():
    a = FakeAgent()
    await a.start()
    payload = b"hello world" * 100_000  # > 1 MiB, multiple chunks
    c = AgentClient("127.0.0.1", a.port, "secret")
    n = await c.write_file("/out.bin", payload)
    assert n == len(payload)
    assert bytes(a.written) == payload
    await a.stop()


@pytest.mark.asyncio
async def test_screenshot_binary(agent):
    c = AgentClient("127.0.0.1", agent.port, "secret")
    jpeg = await c.screenshot()
    assert jpeg.startswith(b"\xff\xd8")


@pytest.mark.asyncio
async def test_error_surfaces(agent):
    c = AgentClient("127.0.0.1", agent.port, "secret")
    with pytest.raises(ClientError) as e:
        await c.call("boom")
    assert e.value.code == "kaboom"


@pytest.mark.asyncio
async def test_malformed_reply_does_not_hang():
    """A garbage/oversized reply frame must raise promptly, not hang the client."""

    async def bad_server(reader, writer):
        # Read the hello, then answer with a bogus length prefix + short body.
        await reader.readexactly(4)
        await reader.read(1024)
        writer.write(struct.pack("<I", 999999))  # claims huge json, sends little
        writer.write(b"{")
        await writer.drain()
        writer.close()

    srv = await asyncio.start_server(bad_server, "127.0.0.1", 0)
    port = srv.sockets[0].getsockname()[1]
    c = AgentClient("127.0.0.1", port, "secret")
    with pytest.raises((ClientError, asyncio.IncompleteReadError, ConnectionError, OSError)):
        await asyncio.wait_for(c.call("sysinfo"), timeout=5)
    srv.close()
    try:
        await asyncio.wait_for(srv.wait_closed(), timeout=2)
    except asyncio.TimeoutError:
        pass


@pytest.mark.asyncio
async def test_reconnect_after_drop():
    a = FakeAgent(drop_first=True)
    await a.start()
    c = AgentClient("127.0.0.1", a.port, "secret")
    # First connection is dropped mid-handshake; client should reconnect.
    reply, _ = await c.call("sysinfo")
    assert reply["ok"] is True
    await a.stop()


# --- retry semantics and deadlines (A2/A3) -----------------------------------


@pytest.mark.asyncio
async def test_non_idempotent_command_not_retried_after_send():
    """A write that reached the agent must not be re-sent: the agent may have
    applied it already, and a second apply is a real side effect."""
    a = FakeAgent(die_on="fs.write")
    await a.start()
    c = AgentClient("127.0.0.1", a.port, "secret")
    with pytest.raises(ClientError) as e:
        await c.call("fs.write", payload=b"data", path="/x", offset=0)
    assert e.value.code == "uncertain"
    assert a.applied.count("fs.write") == 1  # applied once, never duplicated
    await c.aclose()
    await a.stop()


@pytest.mark.asyncio
async def test_idempotent_command_is_retried_after_send():
    """A read is safe to repeat, so a mid-flight drop should be transparent."""

    class RetryAgent(FakeAgent):
        async def _handle(self, reader, writer):
            self._connections += 1
            if self._connections == 1:
                # Die after receiving the first real command post-handshake.
                try:
                    msg, _ = await self._read_frame(reader)
                    await self._write_frame(writer, {"id": msg.get("id"), "ok": True,
                                                     "fw": "18.1.0"})
                    msg, _ = await self._read_frame(reader)
                    self.applied.append(msg.get("cmd"))
                except (asyncio.IncompleteReadError, ConnectionError):
                    pass
                writer.close()
                return
            await super()._handle(reader, writer)

    a = RetryAgent()
    await a.start()
    c = AgentClient("127.0.0.1", a.port, "secret")
    reply, _ = await c.call("sysinfo")  # idempotent: retried on a new connection
    assert reply["ok"] is True
    await c.aclose()
    await a.stop()


@pytest.mark.asyncio
async def test_fire_and_forget_reports_success_without_reply():
    """reboot succeeds precisely by making the console stop answering."""
    a = FakeAgent(die_on="reboot")
    await a.start()
    c = AgentClient("127.0.0.1", a.port, "secret")
    reply, _ = await c.call("reboot")
    assert reply["ok"] is True
    await c.aclose()
    await a.stop()


@pytest.mark.asyncio
async def test_stalled_command_times_out_and_frees_the_lock():
    """A hung command must not hold the connection lock forever, or every other
    tool is frozen behind it."""
    from switch_mcp.clients import agentd as agentd_mod

    a = FakeAgent(stall_on="sysinfo")
    await a.start()
    c = AgentClient("127.0.0.1", a.port, "secret")
    orig = agentd_mod.DEFAULT_TIMEOUT
    agentd_mod.DEFAULT_TIMEOUT = 0.5
    try:
        with pytest.raises((ClientError, asyncio.TimeoutError, OSError)):
            await asyncio.wait_for(c.call("sysinfo"), timeout=10)
        # Lock released: a subsequent call proceeds rather than deadlocking.
        assert not c._lock.locked()
    finally:
        agentd_mod.DEFAULT_TIMEOUT = orig
        await c.aclose()
        await a.stop()


# --- capability negotiation (A11 / B1) ---------------------------------------


@pytest.mark.asyncio
async def test_capabilities_reported_from_handshake():
    a = FakeAgent(tier="observe", emummc=True, commands=["agent.info", "sysinfo"])
    await a.start()
    c = AgentClient("127.0.0.1", a.port, "secret")
    await c.call("sysinfo")
    assert c.capabilities["tier"] == "observe"
    assert c.capabilities["emummc"] is True
    assert c.supports("sysinfo")
    assert not c.supports("i2c.write")  # not advertised at this tier
    await c.aclose()
    await a.stop()


@pytest.mark.asyncio
async def test_older_agent_without_command_list_assumes_support():
    """A pre-0.2.0 agent sends no command list; we must not block on that."""

    class OldAgent(FakeAgent):
        async def _handle(self, reader, writer):
            try:
                while True:
                    msg, _ = await self._read_frame(reader)
                    mid = msg.get("id")
                    if msg.get("cmd") == "hello":
                        await self._write_frame(writer, {"id": mid, "ok": True,
                                                         "fw": "18.1.0"})
                    else:
                        await self._write_frame(writer, {"id": mid, "ok": True})
            except (asyncio.IncompleteReadError, ConnectionError):
                pass
            finally:
                writer.close()

    a = OldAgent()
    await a.start()
    c = AgentClient("127.0.0.1", a.port, "secret")
    await c.call("sysinfo")
    assert c.supports("anything_at_all")
    await c.aclose()
    await a.stop()


# --- challenge-response authentication (A5) ----------------------------------


@pytest.mark.asyncio
async def test_hmac_is_used_and_token_never_crosses_the_wire():
    """The whole point: a passive observer must not learn the token."""
    seen = bytearray()

    class Recording(FakeAgent):
        async def _read_frame(self, reader):
            msg, payload = await super()._read_frame(reader)
            seen.extend(json.dumps(msg).encode())
            return msg, payload

    a = Recording(token="super-secret-token")
    await a.start()
    c = AgentClient("127.0.0.1", a.port, "super-secret-token")
    reply, _ = await c.call("sysinfo")
    assert reply["ok"] is True
    assert c.auth_method == "hmac"
    assert b"super-secret-token" not in bytes(seen), "token leaked in cleartext"
    await c.aclose()
    await a.stop()


@pytest.mark.asyncio
async def test_wrong_token_fails_hmac_auth():
    a = FakeAgent(token="right")
    await a.start()
    c = AgentClient("127.0.0.1", a.port, "wrong")
    with pytest.raises(ClientError) as e:
        await c.call("sysinfo")
    assert e.value.code == "auth"
    await a.stop()


@pytest.mark.asyncio
async def test_nonce_differs_per_connection_so_a_capture_cannot_be_replayed():
    nonces = []

    class Recording(FakeAgent):
        async def _write_frame(self, writer, msg, payload=b""):
            if "challenge" in msg:
                nonces.append(msg["challenge"])
            await super()._write_frame(writer, msg, payload)

    a = Recording()
    await a.start()
    for _ in range(3):
        c = AgentClient("127.0.0.1", a.port, "secret")
        await c.call("sysinfo")
        await c.aclose()
    assert len(nonces) == 3
    assert len(set(nonces)) == 3, "nonce reused — a captured exchange is replayable"
    await a.stop()


@pytest.mark.asyncio
async def test_replaying_a_captured_mac_is_rejected():
    """Replay the exact hmac a legitimate client sent; a fresh nonce must break it."""
    captured = {}

    class Recording(FakeAgent):
        async def _read_frame(self, reader):
            msg, payload = await super()._read_frame(reader)
            if msg.get("cmd") == "auth":
                captured["mac"] = msg["hmac"]
            return msg, payload

    a = Recording()
    await a.start()
    c = AgentClient("127.0.0.1", a.port, "secret")
    await c.call("sysinfo")
    await c.aclose()
    assert "mac" in captured

    # A fresh connection replaying that MAC against a new nonce must fail.
    reader, writer = await asyncio.open_connection("127.0.0.1", a.port)

    async def send(obj):
        raw = json.dumps(obj).encode()
        writer.write(struct.pack("<I", len(raw)) + raw)
        await writer.drain()

    async def recv():
        (jlen,) = struct.unpack("<I", await reader.readexactly(4))
        return json.loads(await reader.readexactly(jlen))

    await send({"id": 1, "cmd": "hello", "auth": "hmac", "version": 1})
    assert "challenge" in await recv()
    await send({"id": 2, "cmd": "auth", "hmac": captured["mac"]})
    assert (await recv()).get("error", {}).get("code") == "auth"
    writer.close()
    await a.stop()


@pytest.mark.asyncio
async def test_falls_back_to_legacy_for_a_pre_0_2_0_agent():
    """An older agent does not understand `auth: hmac`; it must still work."""

    class LegacyAgent(FakeAgent):
        async def _handle(self, reader, writer):
            try:
                while True:
                    msg, _ = await self._read_frame(reader)
                    mid = msg.get("id")
                    if msg.get("cmd") == "hello":
                        # Ignores the auth field entirely, checks the token only.
                        if msg.get("token") != self.token:
                            await self._write_frame(writer, {
                                "id": mid,
                                "error": {"code": "auth", "message": "no"}})
                            break
                        await self._write_frame(writer, {"id": mid, "ok": True,
                                                         "fw": "18.1.0"})
                    else:
                        await self._write_frame(writer, {"id": mid, "ok": True})
            except (asyncio.IncompleteReadError, ConnectionError):
                pass
            finally:
                writer.close()

    a = LegacyAgent(token="secret")
    await a.start()
    c = AgentClient("127.0.0.1", a.port, "secret")
    reply, _ = await c.call("sysinfo")
    assert reply["ok"] is True
    assert c.auth_method == "legacy"
    await c.aclose()
    await a.stop()
