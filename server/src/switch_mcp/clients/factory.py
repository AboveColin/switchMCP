"""Select a client transport from the environment.

SWITCH_MODE=agentd (default) → native switch-agentd protocol (full features).
SWITCH_MODE=fallback         → sys-botbase + sys-ftpd (screen/input/files only).
"""

from __future__ import annotations

import os

from .base import SwitchClient


def make_client() -> SwitchClient:
    mode = os.environ.get("SWITCH_MODE", "agentd").lower()
    if mode in ("fallback", "botbase", "ftp"):
        from .botbase import BotbaseFtpClient

        return BotbaseFtpClient.from_env()
    from .agentd import AgentClient

    return AgentClient.from_env()
