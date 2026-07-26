"""Fallback client using existing homebrew: sys-botbase (screen + input, TCP
6000) for capture/controller, and sys-ftpd (TCP 5000) for files. Lets you use
most MCP tools before building the native sysmodule. System-info, title
management, and save backup aren't available in this mode.
"""

from __future__ import annotations

import asyncio
import os
from ftplib import FTP
from io import BytesIO
from typing import Any

from .base import SwitchClient, Unsupported

# sys-botbase button tokens keyed by our protocol's button names.
_BTN = {
    "A": "A", "B": "B", "X": "X", "Y": "Y",
    "L": "L", "R": "R", "ZL": "ZL", "ZR": "ZR",
    "PLUS": "PLUS", "MINUS": "MINUS",
    "DUP": "DUP", "DDOWN": "DDOWN", "DLEFT": "DLEFT", "DRIGHT": "DRIGHT",
    "LSTICK": "LSTICK", "RSTICK": "RSTICK",
}


class BotbaseFtpClient(SwitchClient):
    def __init__(self, host: str, botbase_port: int = 6000, ftp_port: int = 5000,
                 ftp_user: str = "anonymous", ftp_pass: str = ""):
        self.host = host
        self.botbase_port = botbase_port
        self.ftp_port = ftp_port
        self.ftp_user = ftp_user
        self.ftp_pass = ftp_pass
        self._lock = asyncio.Lock()

    @classmethod
    def from_env(cls) -> "BotbaseFtpClient":
        host = os.environ.get("SWITCH_HOST")
        if not host:
            raise RuntimeError("SWITCH_HOST env var not set (console IP)")
        return cls(
            host,
            int(os.environ.get("SWITCH_BOTBASE_PORT", "6000")),
            int(os.environ.get("SWITCH_FTP_PORT", "5000")),
            os.environ.get("SWITCH_FTP_USER", "anonymous"),
            os.environ.get("SWITCH_FTP_PASS", ""),
        )

    async def _botbase(self, line: str, read_reply: bool = False) -> str:
        async with self._lock:
            reader, writer = await asyncio.wait_for(
                asyncio.open_connection(self.host, self.botbase_port), timeout=5
            )
            try:
                writer.write((line + "\r\n").encode())
                await writer.drain()
                if read_reply:
                    data = await reader.readuntil(b"\n")
                    return data.decode().strip()
                return ""
            finally:
                writer.close()

    async def call(self, cmd: str, payload: bytes = b"", **p: Any):
        if cmd == "screenshot":
            hex_data = await self._botbase("pixelPeek", read_reply=True)
            return {"jpeg": True}, bytes.fromhex(hex_data)

        if cmd == "input":
            tokens = [_BTN[b] for b in p.get("buttons", []) if b in _BTN]
            for t in tokens:
                await self._botbase(f"press {t}")
            await asyncio.sleep(p.get("hold_ms", 100) / 1000)
            for t in tokens:
                await self._botbase(f"release {t}")
            return {"ok": True}, b""

        if cmd == "touch":
            x, y = p.get("x", 0), p.get("y", 0)
            await self._botbase(f"touch {x} {y}")
            return {"ok": True}, b""

        if cmd == "agent.info":
            ver = await self._botbase("getVersion", read_reply=True)
            return {"agent_version": f"sys-botbase {ver}", "mode": "fallback"}, b""

        if cmd.startswith("fs.") or cmd.startswith("save."):
            return await self._ftp(cmd, payload, **p)

        raise Unsupported(cmd)

    # -- FTP-backed filesystem (runs blocking ftplib off the event loop) ------

    async def _ftp(self, cmd: str, payload: bytes, **p: Any):
        return await asyncio.to_thread(self._ftp_sync, cmd, payload, p)

    def _ftp_sync(self, cmd: str, payload: bytes, p: dict):
        ftp = FTP()
        ftp.connect(self.host, self.ftp_port, timeout=10)
        ftp.login(self.ftp_user, self.ftp_pass)
        try:
            if cmd == "fs.list":
                entries = []
                path = p.get("path", "/")
                for name, facts in ftp.mlsd(path):
                    if name in (".", ".."):
                        continue
                    entries.append({
                        "name": name,
                        "type": "dir" if facts.get("type") == "dir" else "file",
                        "size": int(facts.get("size", 0)),
                    })
                return {"entries": entries}, b""
            if cmd == "fs.read":
                buf = BytesIO()
                ftp.retrbinary(f"RETR {p['path']}", buf.write)
                return {"eof": True}, buf.getvalue()
            if cmd == "fs.write":
                ftp.storbinary(f"STOR {p['path']}", BytesIO(payload))
                return {"written": len(payload)}, b""
            if cmd == "fs.delete":
                ftp.delete(p["path"])
                return {"ok": True}, b""
            if cmd == "fs.mkdir":
                ftp.mkd(p["path"])
                return {"ok": True}, b""
            if cmd == "fs.rename":
                ftp.rename(p["from"], p["to"])
                return {"ok": True}, b""
            raise Unsupported(cmd)
        finally:
            ftp.quit()

    async def read_file(self, path: str) -> bytes:
        _, blob = await self.call("fs.read", path=path)
        return blob

    async def write_file(self, path: str, data: bytes) -> int:
        await self.call("fs.write", payload=data, path=path)
        return len(data)
