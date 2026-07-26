"""Tool modules, imported for their registration side effects.

Importing a module runs its decorators, which register with the shared FastMCP
instance in app.py — but only for the groups enabled by SWITCH_MCP_TOOLS. Every
module is imported unconditionally so that a disabled group still fails loudly
on a syntax error rather than silently disappearing.
"""

from . import apps, cheat, core, debug, fs, hardware, resources, system  # noqa: F401

__all__ = ["apps", "cheat", "core", "debug", "fs", "hardware", "resources", "system"]
