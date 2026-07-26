"""Filesystem and save data: browsing, transfers, search and save mounts."""

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


# --- files --------------------------------------------------------------------


@tool("fs")
async def fs_list(path: str = "/", device: str = "sd") -> str:
    """List a directory on the console's SD card ('/' is the SD root)."""
    reply, _ = await client().call("fs.list", path=path, device=device)
    return json.dumps(reply["entries"], indent=2)


@tool("fs")
async def fs_stat(path: str) -> str:
    """Get type/size/mtime for a single file or directory."""
    reply, _ = await client().call("fs.stat", path=path)
    reply.pop("id", None)
    return json.dumps(reply, indent=2)


@tool("fs")
async def fs_mounts() -> str:
    """List the storage mounts the agent exposes (SD is writable; NAND is
    read-only via nand_read)."""
    reply, _ = await client().call("fs.mounts")
    return json.dumps(reply["mounts"], indent=2)


@tool("fs")
async def nand_read(path: str, offset: int = 0, length: int = 1048576, partition: int = 20) -> str:
    """Read up to `length` bytes (max 1 MiB) from a NAND BIS partition,
    read-only. partition 20 = UserDataRoot. Returns base64 (NAND data is
    usually binary). NAND is never written by this agent."""
    reply, blob = await client().call(
        "nand.read", path=path, offset=offset, len=length, partition=partition
    )
    return json.dumps({"eof": reply.get("eof"), "base64": base64.b64encode(blob).decode()})


@tool("fs")
async def fs_download(remote_path: str, local_path: str) -> str:
    """Download a file from the console to the local machine."""
    dest = resolve_local_path(local_path, for_write=True)
    data = await client().read_file(remote_path)
    dest.write_bytes(data)
    return f"Downloaded {len(data)} bytes to {dest}"


@tool("fs")
async def fs_read_text(remote_path: str, max_bytes: int = 65536) -> str:
    """Read a small text file from the console and return its contents inline.
    Use for configs and logs; use fs_download for binaries or large files."""
    data = await client().read_file(remote_path)
    if len(data) > max_bytes:
        data = data[:max_bytes]
    try:
        return data.decode("utf-8", errors="replace")
    except Exception:
        return base64.b64encode(data).decode()


@tool("fs")
async def fs_upload(local_path: str, remote_path: str) -> str:
    """Upload a local file to the console."""
    data = resolve_local_path(local_path, for_write=False).read_bytes()
    await client().write_file(remote_path, data)
    return f"Uploaded {len(data)} bytes to {remote_path}"


@tool("fs")
async def fs_delete(path: str, confirm: str | None = None) -> str:
    """Delete a file or (empty) directory on the console. DESTRUCTIVE: call
    once without `confirm` to get a token, then repeat with it."""
    require_confirmation("fs_delete", f"delete '{path}' from the SD card", confirm)
    await client().call("fs.delete", path=path)
    return f"Deleted {path}"


@tool("fs")
async def fs_mkdir(path: str) -> str:
    """Create a directory on the console."""
    await client().call("fs.mkdir", path=path)
    return f"Created {path}"


@tool("fs")
async def fs_rename(from_path: str, to_path: str) -> str:
    """Rename/move a file or directory on the console."""
    await client().call("fs.rename", **{"from": from_path, "to": to_path})
    return f"Renamed {from_path} -> {to_path}"


@tool("fs")
async def list_saves(type: str = "", limit: int = 400) -> str:
    """Enumerate EVERY save on the console — game saves per user, system saves,
    BCAT, device and cache storage — with type, owner, size and IDs.

    This is the discovery step: it needs no title ID up front, unlike
    backup_save. Filter with type = account | system | bcat | device | cache |
    temporary. Account saves carry an application_id and a user uid; system
    saves carry a system_save_data_id instead (e.g. ...8000000000000010 is
    system settings, ...30 accounts, ...47 the ticket DB).

    Feed the IDs into mount_save to browse a save's contents in place."""
    reply, _ = await client().call("save.list", type=type, limit=limit)
    reply.pop("id", None)
    return json.dumps(reply, indent=2)


@tool("fs")
async def mount_save(title_id: str = "", system_save_data_id: str = "",
                     uid_hi: str = "0", uid_lo: str = "0", space: str = "") -> str:
    """Mount a save READ-ONLY so it can be browsed in place.

    Afterwards use fs_list / fs_read_text / fs_download with device="save" to
    explore it — no copying to the SD card first, unlike backup_save.

    Give either title_id (a game save; add uid_hi/uid_lo from list_saves for a
    specific user) or system_save_data_id (a system save). One save is mounted
    at a time; mounting again replaces the previous one.

    Pass `space` from the list_saves entry for a system save; without it the
    plausible spaces are tried in order.

    The mount is read-only, and every write command refuses a non-SD device, so
    nothing here can corrupt save data. Use restore_save to write a save back."""
    reply, _ = await client().call(
        "save.mount", tid=title_id, system_save_data_id=system_save_data_id,
        uid_hi=uid_hi, uid_lo=uid_lo, space=space,
    )
    reply.pop("id", None)
    return json.dumps(reply, indent=2)


@tool("fs")
async def unmount_save() -> str:
    """Release the read-only save mount from mount_save."""
    await client().call("save.unmount")
    return "Save unmounted"


@tool("fs")
async def fs_free_space(device: str = "sd") -> str:
    """Free, used and total bytes for a device ("sd", or "save" when mounted).
    Check this before a download, upload or dump."""
    reply, _ = await client().call("fs.freespace", device=device)
    reply.pop("id", None)
    return json.dumps(reply, indent=2)


@tool("fs")
async def fs_find(path: str = "/", name_contains: str = "", files_only: bool = False,
                  limit: int = 200, device: str = "sd") -> str:
    """Recursively search for files/directories by name, ON the console.

    Vastly cheaper than listing directories over the wire and filtering here —
    an SD card full of homebrew is thousands of round-trips otherwise. Matching
    is case-insensitive substring; an empty name_contains lists everything under
    `path`.

    The walk is depth- and scan-capped, and the reply says explicitly whether it
    was truncated (`truncated_by_limit` / `truncated_by_scan_cap`) rather than
    quietly returning a partial answer."""
    reply, _ = await client().call(
        "fs.find", path=path, name_contains=name_contains, files_only=files_only,
        limit=limit, device=device,
    )
    reply.pop("id", None)
    return json.dumps(reply, indent=2)


@tool("fs")
async def fs_grep(path: str, pattern: str, ignore_case: bool = False,
                  limit: int = 100, device: str = "sd") -> str:
    """Search inside a file on the console, returning matching lines.

    Returns line number, byte offset and text per match instead of transferring
    the file. Ideal for config files and logs — much cheaper than fs_read_text
    on anything large. Long lines are clipped in the reply."""
    reply, _ = await client().call(
        "fs.grep", path=path, pattern=pattern, ignore_case=ignore_case,
        limit=limit, device=device,
    )
    reply.pop("id", None)
    return json.dumps(reply, indent=2)


@tool("fs")
async def backup_save(title_id: str) -> str:
    """Back up a game's save data to sd:/switch-agentd/saves/<title_id>.
    Uses the primary user account."""
    reply, _ = await client().call("save.backup", tid=title_id)
    return f"Backed up save to {reply.get('path')}"


@tool("fs")
async def restore_save(title_id: str, confirm: str | None = None) -> str:
    """Restore a previously backed-up save for a title back into its save data.
    Commits the write to NAND and OVERWRITES the current save. Call once without
    `confirm` to get a token, then repeat with it."""
    require_confirmation(
        "restore_save",
        f"overwrite the live save data for title {title_id} with the SD backup "
        f"(current in-game progress since that backup is lost)",
        confirm,
    )
    await client().call("save.restore", tid=title_id)
    return f"Restored save for {title_id}"
