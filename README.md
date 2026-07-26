# switchMCP

Drive a homebrew Nintendo Switch from an AI agent, over MCP.

`[Atmosphère only]` `[tested on HOS 22.1.0]` `[emuMMC and sysMMC]`

Two parts:

- **`agent/`** — `switch-agentd`, a C++ sysmodule that runs in the background on
  the console and exposes a control protocol on TCP 6060.
- **`server/`** — a Python MCP server that runs on your computer, turning that
  protocol into tools an LLM can call.

```
Claude ── MCP/stdio ── server/ (Python) ── TCP 6060 (Wi-Fi) ── agent/ (sysmodule)
```

A fresh install is read-only: the agent starts at the `observe` tier and cannot
change anything on the console until you raise it deliberately.

---

## Features

### Eyes and hands
- Screen capture, scaled by default to ~20% of the native JPEG size — the same
  menu is readable at a quarter of the tokens
- `screen_changed`, which waits for the UI to settle and returns **one** image
  only if something actually changed
- Controller and touch input, macro sequences, virtual USB keyboard
- Read the *physical* controller, and record real input into a replayable macro

### Files and saves
- Browse, transfer, hash and search the SD card — `find` and `grep` run **on the
  console**, so you are not shipping a directory tree over Wi-Fi to filter it
- Enumerate **every** save on the system (game, system, BCAT, device, cache)
  without knowing a title ID first
- Mount any save **read-only** and read it in place

### Diagnostics
- One-shot triage: system info, running app, crash reports, screenshot
- Atmosphère crash *and* fatal reports
- Process list, module list with build IDs, network configuration
- `preflight` — battery, charger, free space, emuMMC vs sysMMC — before anything
  irreversible

### Debugging and memory
- Attach a debugger: memory map, threads, registers, backtraces
- Module base + build ID, so an address becomes `main+0x1a2f4` for Ghidra/IDA
- Read and write memory of a **running, unpaused** game via `dmnt:cht`
- Narrowing value search (`find_value` → `narrow_search`) with candidates held on
  the console, so each step costs one round-trip

### Safety
- Three capability tiers, enforced before any handler runs
- `dry_run` on every mutating command
- Destructive tools require a two-call confirmation token bound to the exact
  effect
- On-device audit journal, and a watchdog that counts boots with no client

Full tool reference: **[Tools](https://github.com/AboveColin/switchMCP/wiki/Tools)**. Worked recipes: **[Examples](https://github.com/AboveColin/switchMCP/wiki/Examples)**.

---

## Known limitations

Please read these before filing an issue — all are documented behaviour.

- **Hardware watchpoints and breakpoints arm but never fire.** The syscall is
  permitted, the control-word encoding is accepted, slots allocate and clear —
  but no exception is ever delivered on HOS 22.1.0. Core affinity was tested as
  a cause and ruled out. Use the narrowing memory search instead; that works.
- **USB transport is inert.** The device side compiles and is off by default.
  There is no host client and it has never been tested against hardware.
- **One client at a time** — not one command, one *session*. The agent does not
  reach `accept()` again while a client holds a connection, so a second client
  blocks until the first disconnects. A long `wait_event` blocks everything.
- **Register writes need a thread stopped at a debug event.** A paused attach is
  not sufficient; the kernel returns `InvalidState`. That is the normal debugger
  contract, not a bug.
- **Throughput is limited by Wi-Fi, not the protocol**: measured 2.8 MB/s up,
  4.1 MB/s down, 22.8 ms round-trip.
- Homebrew cannot be launched by title ID. `launch_title` refuses the hbloader
  ID, because doing so spawns a process that renders nothing and then blocks the
  real launch path.

---

## Installing

1. Download the latest release zip.
2. Extract it to the **root of your SD card**, letting the folders merge:

```
atmosphere/contents/420000000000AE57/exefs.nsp
atmosphere/contents/420000000000AE57/flags/boot2.flag   <- empty; makes it auto-start
atmosphere/contents/420000000000AE57/toolbox.json
config/switch-agentd/config.ini.example
```

3. Reboot the console.

On first boot the agent writes `sd:/config/switch-agentd/config.ini` with a
**randomly generated token**. Read that file to get the token — you need it to
connect. It never crosses the network: the client proves knowledge of it by
answering an HMAC challenge.

**Success check:** `sd:/config/switch-agentd/agent.log` should end with
`listening on port 6060`. No log at all means the module is not starting — see
[Troubleshooting](https://github.com/AboveColin/switchMCP/wiki/Troubleshooting).

### Connecting an agent

```bash
cd server && uv sync
claude mcp add switch \
  -e SWITCH_HOST=<console-ip> -e SWITCH_TOKEN=<token-from-config.ini> \
  -- uv --directory $(pwd) run switch-mcp
```

`SWITCH_MCP_TOOLS` picks which tool groups are registered: unset gives 69 tools,
`all` gives 102, `core` gives 15. See **[Configuration](https://github.com/AboveColin/switchMCP/wiki/Configuration)**.

---

## Configuring

`sd:/config/switch-agentd/config.ini`. The important setting:

```ini
# observe  - read-only. Cannot change anything. (default)
# control  - what a person holding the console could do: input, launch apps,
#            SD-card files, reboot. Nothing here risks the firmware.
# invasive - NAND/BIS writes, raw i2c, live memory patching, uninstalls.
tier = observe
```

Reaching `invasive` is necessary but not sufficient for the sharpest edges:
`allow_nand_write`, `allow_overclock` and `allow_hardware` are separate opt-ins.

emuMMC is detected automatically and reported in the handshake. On emuMMC a
mistake is recoverable by restoring your SD image; on sysMMC it may not be.

---

## Removing

Delete these and reboot:

```
sd:/atmosphere/contents/420000000000AE57/
sd:/config/switch-agentd/
sd:/switch-agentd/            (save backups and the journal — safe to keep)
```

If a bad build stops the console booting, hold **Volume-Up** during boot to skip
boot2 sysmodules, then delete the contents folder.

---

## Building

Needs [devkitPro](https://devkitpro.org/wiki/Getting_Started) with `switch-dev`.

```bash
sudo dkp-pacman -S switch-dev

make -C agent              # builds switch-agentd.nsp
make -C agent dist         # assembles the SD-card release zip
make -C agent host-tests   # unit tests for the JSON codec and path policy;
                           # runs natively — no devkitPro or console needed

cd server && uv sync --extra dev && uv run pytest
```

The host tests build `json.cpp` and `path.cpp` under ASan/UBSan. They cover the
code every request passes through, including the path policy that stops
directory traversal — worth running before any cross-build.

---

## A note on stability

This softly bricked a console three times during development. All three were
recoverable and all are fixed. They are worth recording because the failure
modes generalise to any sysmodule:

- **64 KiB stack buffers on a 32 KiB stack.** GCC reserves the frame in the
  prologue, so the module died on function *entry*, before any of its own code
  ran.
- **Registering with `psc` without answering it.** A power-state module that
  fails to acknowledge a sleep request hangs the console — black screen, no
  crash report.
- **Raising `INNER_HEAP_SIZE` to 16 MiB.** A sysmodule's heap comes from a
  constrained system pool; the allocation failed during init and the module died
  before it could log anything. There is now a `static_assert` against it.

The last two produce **no log and no crash report at all**, which is why the
boot watchdog and the on-device journal exist.

---

## Disclaimer

This is a tool for managing your own console and developing homebrew. Running
custom firmware and connecting to Nintendo's online services carries a non-zero
ban risk, and this project makes no attempt to hide itself. The authors are not
liable for any damage, data loss or bans received. **Use at your own risk.**

Installing titles is out of scope and not implemented.

---

## Credits

Built on [libnx](https://github.com/switchbrew/libnx) and
[Atmosphère](https://github.com/Atmosphere-NX/Atmosphere). Thanks to switchbrew,
devkitPro, SciresM and the ReSwitched community — the protocol work here leans
heavily on their documentation.

Licensed under [GPLv3](LICENSE).
