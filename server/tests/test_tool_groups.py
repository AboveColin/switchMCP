"""Tests for tool-group gating (A8).

The point of grouping is that a default session is not handed ~93 tools, most
of them irrelevant to it. These check the gate actually gates, and that
misconfiguration degrades safely rather than leaving a server with no tools.

Each case runs in a subprocess: registration happens at import time against a
module-level FastMCP instance, so reloading in-process would accumulate state
across cases and prove nothing.
"""

from __future__ import annotations

import json
import os
import subprocess
import sys

import pytest

from switch_mcp.app import ALL_GROUPS, DEFAULT_GROUPS, enabled_groups

SNIPPET = """
import asyncio, json
from switch_mcp import server
print(json.dumps(sorted(t.name for t in asyncio.run(server.mcp.list_tools()))))
"""


def tools_with(value: str | None) -> set[str]:
    env = dict(os.environ, SWITCH_HOST="127.0.0.1")
    env.pop("SWITCH_MCP_TOOLS", None)
    if value is not None:
        env["SWITCH_MCP_TOOLS"] = value
    out = subprocess.run([sys.executable, "-c", SNIPPET], env=env,
                         capture_output=True, text=True, check=True)
    return set(json.loads(out.stdout.strip().splitlines()[-1]))


# --- the pure selection function (fast, no subprocess) -----------------------


def test_unset_gives_the_default_groups(monkeypatch):
    monkeypatch.delenv("SWITCH_MCP_TOOLS", raising=False)
    assert enabled_groups() == DEFAULT_GROUPS


def test_all_selects_every_group(monkeypatch):
    monkeypatch.setenv("SWITCH_MCP_TOOLS", "all")
    assert enabled_groups() == ALL_GROUPS


def test_unknown_names_fall_back_rather_than_disabling_everything(monkeypatch):
    """A typo must never leave the server with no tools at all."""
    monkeypatch.setenv("SWITCH_MCP_TOOLS", "bogus,alsobogus")
    assert enabled_groups() == DEFAULT_GROUPS


def test_partially_valid_list_keeps_the_valid_part(monkeypatch):
    monkeypatch.setenv("SWITCH_MCP_TOOLS", "core, notathing ,debug")
    assert set(enabled_groups()) == {"core", "debug"}


def test_whitespace_and_case_are_tolerated(monkeypatch):
    monkeypatch.setenv("SWITCH_MCP_TOOLS", "  CORE , Debug  ")
    assert set(enabled_groups()) == {"core", "debug"}


def test_deep_tiers_are_not_on_by_default():
    assert "debug" not in DEFAULT_GROUPS
    assert "cheat" not in DEFAULT_GROUPS
    assert "hardware" not in DEFAULT_GROUPS


# --- end-to-end registration -------------------------------------------------


@pytest.mark.slow
def test_registration_matches_the_selected_groups():
    everything = tools_with("all")
    default = tools_with(None)
    core = tools_with("core")

    # Deliberately no hardcoded total: pinning the count means every new tool
    # breaks this test for no reason. What matters is the containment ordering.
    assert core < default < everything
    assert len(core) < 25

    # Deep-tier tools must be absent by default and present when asked for.
    for deep in ("debug_attach", "i2c_write", "find_value"):
        assert deep not in default
        assert deep in everything

    # Whatever is trimmed, a core session can still see and act.
    for essential in ("ping", "capture_screen", "send_input", "tap"):
        assert essential in core

    # Adding a group is purely additive: it must not drop or alter anything.
    core_debug = tools_with("core,debug")
    assert core < core_debug
    assert core_debug <= everything
    assert "debug_attach" in core_debug


@pytest.mark.slow
def test_bogus_value_registers_the_default_set():
    assert tools_with("bogus") == tools_with(None)
