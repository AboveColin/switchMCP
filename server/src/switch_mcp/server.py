"""MCP server exposing remote-management tools for a homebrew Switch.

Every tool proxies to the on-device switch-agentd over the wire protocol in
the documented wire protocol. The tools themselves live in `tools/`, grouped so a session
can expose only what it needs — see app.py for the grouping and why.

Tool docstrings are written to teach an LLM operator how to use each tool
effectively (coordinate system, button names, title-ID format, and what to do
when something is refused).
"""

from __future__ import annotations

import os

from .app import enabled_groups, mcp
from .guards import check_bind_safety

# Imported for the registration side effect; must come after `mcp` exists.
from . import tools  # noqa: F401,E402


def main() -> None:
    # SWITCH_MCP_TRANSPORT: stdio (default, one agent spawns it locally),
    # streamable-http or sse (one shared instance many/remote agents connect to).
    # Exits rather than exposing an unauthenticated control surface off-box.
    check_bind_safety()
    transport = os.environ.get("SWITCH_MCP_TRANSPORT", "stdio")
    mcp.run(transport=transport)


if __name__ == "__main__":
    main()
