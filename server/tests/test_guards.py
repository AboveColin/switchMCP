"""Tests for the server-side safety gates (guards.py)."""

from __future__ import annotations

import os

import pytest

from switch_mcp import guards
from switch_mcp.guards import Blocked, require_confirmation, resolve_local_path


@pytest.fixture(autouse=True)
def clean_env(monkeypatch):
    for var in ("SWITCH_MCP_ALLOW_DESTRUCTIVE", "SWITCH_MCP_LOCAL_ROOTS",
                "SWITCH_MCP_TRANSPORT", "SWITCH_MCP_ALLOW_REMOTE", "SWITCH_MCP_HOST"):
        monkeypatch.delenv(var, raising=False)
    guards._pending.clear()


def test_first_call_is_blocked_and_issues_a_token():
    with pytest.raises(Blocked) as e:
        require_confirmation("reboot", "reboot the console", None)
    assert "DESTRUCTIVE" in str(e.value)
    assert len(guards._pending) == 1


def test_token_allows_exactly_one_execution():
    with pytest.raises(Blocked) as e:
        require_confirmation("reboot", "reboot the console", None)
    token = next(iter(guards._pending))
    assert token in str(e.value)

    require_confirmation("reboot", "reboot the console", token)  # proceeds

    # Token is single-use: replaying it must not work.
    with pytest.raises(Blocked, match="unknown or expired"):
        require_confirmation("reboot", "reboot the console", token)


def test_token_is_bound_to_the_specific_effect():
    """A token issued to delete one file must not authorise deleting another."""
    with pytest.raises(Blocked):
        require_confirmation("fs_delete", "delete '/harmless.txt'", None)
    token = next(iter(guards._pending))
    with pytest.raises(Blocked, match="does not match"):
        require_confirmation("fs_delete", "delete '/atmosphere/package3'", token)


def test_unknown_token_rejected():
    with pytest.raises(Blocked, match="unknown or expired"):
        require_confirmation("reboot", "reboot the console", "deadbeef1234")


def test_expired_token_rejected(monkeypatch):
    with pytest.raises(Blocked):
        require_confirmation("reboot", "reboot the console", None)
    token = next(iter(guards._pending))
    monkeypatch.setattr(guards, "_TOKEN_TTL_SECONDS", 0.0)
    import time as _t
    detail, _ = guards._pending[token]
    guards._pending[token] = (detail, _t.time() - 1)  # already expired
    with pytest.raises(Blocked, match="unknown or expired"):
        require_confirmation("reboot", "reboot the console", token)


def test_env_override_skips_confirmation(monkeypatch):
    monkeypatch.setenv("SWITCH_MCP_ALLOW_DESTRUCTIVE", "1")
    require_confirmation("reboot", "reboot the console", None)  # no raise


# --- local path bounds -------------------------------------------------------


def test_paths_unrestricted_by_default(tmp_path):
    p = resolve_local_path(str(tmp_path / "out.bin"), for_write=True)
    assert p == tmp_path / "out.bin"


def test_root_allowlist_enforced(tmp_path, monkeypatch):
    allowed = tmp_path / "ok"
    allowed.mkdir()
    monkeypatch.setenv("SWITCH_MCP_LOCAL_ROOTS", str(allowed))
    resolve_local_path(str(allowed / "f.bin"), for_write=True)  # inside: fine
    with pytest.raises(Blocked, match="outside the allowed local roots"):
        resolve_local_path(str(tmp_path / "elsewhere.bin"), for_write=True)


def test_traversal_cannot_escape_a_root(tmp_path, monkeypatch):
    allowed = tmp_path / "ok"
    allowed.mkdir()
    monkeypatch.setenv("SWITCH_MCP_LOCAL_ROOTS", str(allowed))
    with pytest.raises(Blocked):
        resolve_local_path(str(allowed / ".." / "escape.bin"), for_write=True)


def test_symlink_cannot_escape_a_root(tmp_path, monkeypatch):
    allowed = tmp_path / "ok"
    allowed.mkdir()
    secret = tmp_path / "secret.txt"
    secret.write_text("private")
    link = allowed / "link.txt"
    link.symlink_to(secret)
    monkeypatch.setenv("SWITCH_MCP_LOCAL_ROOTS", str(allowed))
    with pytest.raises(Blocked):
        resolve_local_path(str(link), for_write=False)


def test_remote_transport_blocks_unbounded_host_writes(monkeypatch):
    monkeypatch.setenv("SWITCH_MCP_TRANSPORT", "streamable-http")
    with pytest.raises(Blocked, match="disabled over a network transport"):
        resolve_local_path("/tmp/anywhere.bin", for_write=True)


# --- bind safety -------------------------------------------------------------


def test_stdio_bind_always_allowed():
    guards.check_bind_safety()


def test_loopback_http_allowed(monkeypatch):
    monkeypatch.setenv("SWITCH_MCP_TRANSPORT", "streamable-http")
    monkeypatch.setenv("SWITCH_MCP_HOST", "127.0.0.1")
    guards.check_bind_safety()


def test_routable_bind_refused(monkeypatch):
    monkeypatch.setenv("SWITCH_MCP_TRANSPORT", "streamable-http")
    monkeypatch.setenv("SWITCH_MCP_HOST", "0.0.0.0")
    with pytest.raises(SystemExit, match="Refusing to bind"):
        guards.check_bind_safety()


def test_routable_bind_allowed_with_explicit_optin(monkeypatch):
    monkeypatch.setenv("SWITCH_MCP_TRANSPORT", "streamable-http")
    monkeypatch.setenv("SWITCH_MCP_HOST", "0.0.0.0")
    monkeypatch.setenv("SWITCH_MCP_ALLOW_REMOTE", "1")
    guards.check_bind_safety()


# --- actionable errors (A10) -------------------------------------------------


def test_error_carries_actionable_guidance():
    """A bare code tells the model nothing; it must say what to change."""
    from switch_mcp.clients.base import ClientError

    e = ClientError("tier_denied", "'uninstall' requires tier=invasive")
    assert "tier_denied" in str(e)
    assert "config.ini" in str(e)          # names the thing to change
    assert e.guidance is not None
    assert e.raw_message == "'uninstall' requires tier=invasive"


def test_unknown_codes_still_work_without_guidance():
    from switch_mcp.clients.base import ClientError

    e = ClientError("weird_new_code", "something happened")
    assert str(e) == "weird_new_code: something happened"
    assert e.guidance is None
    assert e.code == "weird_new_code"


def test_uncertain_guidance_warns_against_blind_retry():
    from switch_mcp.clients.base import ClientError

    e = ClientError("uncertain", "fs.write was sent but the reply was lost")
    assert "may or may not have taken effect" in str(e)


def test_every_guidance_entry_is_actionable_prose():
    """Guidance should tell the operator what to do, not restate the error."""
    from switch_mcp.clients.base import ERROR_GUIDANCE

    for code, text in ERROR_GUIDANCE.items():
        assert len(text) > 40, f"{code} guidance is too thin to act on"
        assert text[0].isupper(), f"{code} guidance should read as a sentence"
