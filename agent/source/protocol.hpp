// Frame protocol, config, and command-handler interface for switch-agentd.
// Wire format: length-prefixed JSON frames, with binary payloads following
// the JSON header as raw bytes. Defined by this file and server.cpp.
#pragma once

#include <switch.h>

#include <cstdint>
#include <string>
#include <vector>

#include "json.hpp"
#include "path.hpp"   // NormalizePath / ResolveSdPath (host-testable)

// Capability tiers. Each command declares the tier it needs; the dispatcher
// refuses anything above the configured level. Read-only by default, so a fresh
// install cannot change device state until someone opts in.
//
//   Observe  — read state. Cannot alter the console in any way.
//   Control  — drive the console as a user could: input, launch apps, SD files.
//              Nothing here risks the firmware.
//   Invasive — writes that can leave the console unbootable or damage hardware:
//              NAND/BIS writes, raw i2c, debug memory patching, uninstalls.
enum class Tier : int {
    Observe = 0,
    Control = 1,
    Invasive = 2,
};

struct AgentConfig {
    uint16_t port = 6060;
    std::string token;          // shared secret checked in the hello frame
    int log_level = 2;          // 0=error 1=warn 2=info 3=debug
    Tier tier = Tier::Observe;  // default: cannot change anything

    // Sub-gates within the invasive tier. Being invasive is necessary but not
    // sufficient for these; each is an independent, deliberate opt-in.
    bool allow_nand_write = false;   // gate risky NAND writes (default off)
    bool allow_overclock = false;    // gate clock changes (default off)
    bool allow_hardware = false;     // gate raw i2c/gpio access (default off)

    // Clear the boot lockscreen automatically. On by default: the lockscreen
    // auto-sleeps even with auto-sleep set to Never, and sleeping drops the
    // network, so leaving it alone makes a headless console unreachable after
    // every reboot. Needs tier >= control, since it injects input.
    bool clear_lockscreen_on_boot = true;

    // Minutes between keep-awake nudges, or 0 to disable. Auto-sleep set to
    // Never still lets the console re-lock when idle, and the lockscreen sleeps
    // by itself, dropping the network. Nudges are skipped while a game is
    // running so they can never disturb play.
    int keep_awake_minutes = 10;

    // Refuse the legacy cleartext-token handshake. Off by default so existing
    // clients keep working; turn it on once every client speaks HMAC.
    bool require_hmac_auth = false;

    // Register with psc:m for sleep/wake notifications. OFF by default: a
    // registered module that fails to acknowledge a sleep request HANGS THE
    // CONSOLE, which is exactly what an earlier buggy version did. The
    // accept()-error fallback recovers a few seconds slower and cannot hang
    // anything, so it is the safe default.
    bool enable_psc = false;

    // USB bulk transport (usb:ds). OFF and UNVERIFIED — no hardware test has
    // been possible (needs a cable and a host client). Initialised lazily, never
    // at boot, so a failure here can never stop the sysmodule starting.
    bool enable_usb = false;

    // True when running under emuMMC. A bricked emuMMC is recovered by
    // re-imaging the SD card, so invasive operations are far less consequential
    // there; on sysMMC the same mistake can mean a dead console. Detected at
    // boot, not configurable.
    bool is_emummc = false;
};

namespace agent {

AgentConfig LoadConfig(const char* path);

// A parsed request plus its attached binary payload (empty if none).
struct Request {
    const json::Value& msg;                 // full JSON message object
    const std::vector<uint8_t>& payload;    // binary attached to request
    const AgentConfig& cfg;

    const json::Value& operator[](const std::string& k) const { return msg[k]; }
};

// A handler's reply: a JSON object (the server injects `id` and, if `out` is
// non-empty, the `bin` field + binary trailer) and an optional binary payload.
struct Reply {
    json::Value json = json::Value::object();
    std::vector<uint8_t> out;
};

// Handlers return true on success. On false they should have populated
// `reply.json` with {"error": {...}} via Fail(); the server sends it as-is.
using Handler = bool (*)(const Request&, Reply&);

// A command plus the authority it needs. Keeping the tier next to the handler
// in one table means adding a command forces a decision about its blast radius,
// rather than defaulting to fully permitted.
struct Command {
    Handler fn;
    Tier tier;
    // Mutates device state, so it must honour a dry_run request. Commands
    // marked here are answered with a preview instead of acting when the caller
    // passes dry_run:true.
    bool mutating;
};

// True when the caller asked for a preview rather than execution. Mutating
// handlers do not need to check this themselves — the dispatcher intercepts —
// but handlers with a more informative preview may.
bool IsDryRun(const Request& req);

// Helper for handlers: set a structured error and return false.
bool Fail(Reply& reply, const char* code, const char* message);
bool Fail(Reply& reply, const char* code, const std::string& message);


// fsdev device name used for a read-only save mount.
inline constexpr const char* kSaveMountName = "save_ro";
extern bool g_save_mounted;

// Same policy, but for a named logical device. "sd" is the SD card and is
// writable; "save" is a read-only save mount (see save.mount). Unknown or
// unmounted devices are rejected. `writable` reports whether the resolved
// device tolerates mutation, so callers can refuse a write instead of failing
// deep inside libnx with a confusing error.
bool ResolveDevicePath(const std::string& device, const std::string& in,
                       std::string& out, bool* writable);

// ---- command handlers (implemented in handlers/*.cpp) ----------------------
namespace handlers {
bool Screenshot(const Request&, Reply&);        // capssc → JPEG
bool FsList(const Request&, Reply&);
bool FsRead(const Request&, Reply&);
bool FsWrite(const Request&, Reply&);
bool FsDelete(const Request&, Reply&);
bool FsMkdir(const Request&, Reply&);
bool FsRename(const Request&, Reply&);
bool FsStat(const Request&, Reply&);
bool FsMounts(const Request&, Reply&);
bool FsHash(const Request&, Reply&);
bool FsFreeSpace(const Request&, Reply&);       // free/total per device
bool FsFind(const Request&, Reply&);            // recursive name search on-device
bool FsGrep(const Request&, Reply&);            // content search on-device
bool SaveList(const Request&, Reply&);          // enumerate ALL save data
bool SaveMount(const Request&, Reply&);         // mount a save read-only
bool SaveUnmount(const Request&, Reply&);
bool NandRead(const Request&, Reply&);
bool BackupSave(const Request&, Reply&);
bool RestoreSave(const Request&, Reply&);
bool SysInfo(const Request&, Reply&);           // setsys/psm/ts/nifm/fs/…
bool ProcessList(const Request&, Reply&);       // pm:dmnt
bool CrashReports(const Request&, Reply&);
bool ReadLog(const Request&, Reply&);
bool Titles(const Request&, Reply&);            // ns
bool TitleIcon(const Request&, Reply&);
bool RunningApp(const Request&, Reply&);
bool Launch(const Request&, Reply&);            // pmshell / ns
bool Terminate(const Request&, Reply&);
bool Uninstall(const Request&, Reply&);         // ns delete application
bool Input(const Request&, Reply&);             // hiddbg HDLS buttons/sticks
bool Touch(const Request&, Reply&);             // hiddbg touchscreen
bool TypeText(const Request&, Reply&);          // hiddbg keyboard autopilot
bool Reboot(const Request&, Reply&);            // bpc
bool Shutdown(const Request&, Reply&);
bool RebootToPayload(const Request&, Reply&);   // bpc:ams
bool SetBrightness(const Request&, Reply&);     // lbl
bool SetClocks(const Request&, Reply&);         // clkrst (gated)
bool SetSleep(const Request&, Reply&);          // setsys auto-sleep plans
bool AgentRestart(const Request&, Reply&);      // reboot to reload module
bool AgentInfo(const Request&, Reply&);
// Additional system tools (handlers/system.cpp)
bool GetBrightness(const Request&, Reply&);     // lbl
bool SetAutoBrightness(const Request&, Reply&); // lbl
bool GetVolume(const Request&, Reply&);         // audctl
bool SetVolume(const Request&, Reply&);         // audctl
bool SetWireless(const Request&, Reply&);       // nifm
bool GetTime(const Request&, Reply&);           // time
bool SetTime(const Request&, Reply&);           // time
bool ConsoleInfo(const Request&, Reply&);       // setsys/set
bool FsCopy(const Request&, Reply&);
bool GetSleep(const Request&, Reply&);          // setsys sleep plans (read)
bool SettingsGet(const Request&, Reply&);       // arbitrary settings item (read)
bool GetClocks(const Request&, Reply&);         // pcv read
bool Controllers(const Request&, Reply&);       // hid npad power/connection
bool AlbumList(const Request&, Reply&);         // capsa
bool AlbumDownload(const Request&, Reply&);     // capsa
// Live debugger (handlers/debug.cpp) — Atmosphère debug syscalls
bool DebugAttach(const Request&, Reply&);
bool DebugDetach(const Request&, Reply&);
bool DebugReadMem(const Request&, Reply&);
bool DebugWriteMem(const Request&, Reply&);
bool DebugMemMap(const Request&, Reply&);
bool DebugThreads(const Request&, Reply&);
bool DebugRegisters(const Request&, Reply&);
bool DebugSearch(const Request&, Reply&);
bool DebugPollEvents(const Request&, Reply&);   // svcGetDebugEvent stream
bool DebugContinue(const Request&, Reply&);
bool DebugBreak(const Request&, Reply&);
bool DebugSetWatchpoint(const Request&, Reply&);    // svcSetHardwareBreakPoint
bool DebugSetBreakpoint(const Request&, Reply&);
bool DebugClearBreakpoint(const Request&, Reply&);
bool DebugWriteRegisters(const Request&, Reply&);
bool DebugSetHwBpRaw(const Request&, Reply&);       // raw svcSetHardwareBreakPoint
bool DebugModules(const Request&, Reply&);          // ldr:dmnt module + build IDs
bool DebugBacktrace(const Request&, Reply&);        // fp-chain stack walk   // svcSetDebugThreadContext
// Cheat engine (handlers/cheat.cpp) — dmnt:cht raw IPC
bool CheatStatus(const Request&, Reply&);
bool CheatList(const Request&, Reply&);
bool CheatToggle(const Request&, Reply&);
bool FreezeAddress(const Request&, Reply&);
bool UnfreezeAddress(const Request&, Reply&);
// Live memory + narrowing search (handlers/memsearch.cpp) — dmnt:cht, no pause
bool CheatReadMem(const Request&, Reply&);
bool CheatWriteMem(const Request&, Reply&);
bool CheatMeta(const Request&, Reply&);
bool CheatMappings(const Request&, Reply&);
// Events + macro recording (handlers/events.cpp)
bool WaitEvent(const Request&, Reply&);
bool RecordInput(const Request&, Reply&);
bool ReadInput(const Request&, Reply&);
bool NetInfo(const Request&, Reply&);           // nifm IP config
bool FatalReports(const Request&, Reply&);      // Atmosphère fatal reports
// Brick guards (handlers/guards.cpp)
bool PreflightCheck(const Request&, Reply&);    // battery/space/emuMMC safety
bool JournalRead(const Request&, Reply&);
bool WatchdogStatus(const Request&, Reply&);
void JournalAppend(const char* op, const std::string& detail);
void WatchdogOnBoot();
void WatchdogOnConnect();
bool SearchBegin(const Request&, Reply&);
bool SearchNext(const Request&, Reply&);
bool SearchResults(const Request&, Reply&);
bool SearchReset(const Request&, Reply&);
// Hardware buses (handlers/hardware.cpp) — gated by allow_hardware
bool I2cRead(const Request&, Reply&);
bool I2cWrite(const Request&, Reply&);
bool GpioRead(const Request&, Reply&);
}  // namespace handlers

class Server {
   public:
    explicit Server(const AgentConfig& cfg) : cfg_(cfg) {}

    // Blocking: sets up the listener and accepts one client at a time. Never
    // returns under normal operation; logs and continues on per-connection
    // errors so a bad client cannot take down the module.
    void Run();

   private:
    void ServeConnection(int fd);
    bool Authenticate(int fd);

    AgentConfig cfg_;
    bool authed_ = false;
};

// ---- USB bulk transport (usb_transport.cpp) --------------------------------
// Same frame protocol, different byte pump. UNVERIFIED; see the file header.
namespace usb_transport {
bool Init(const AgentConfig& cfg);   // lazy; safe to call repeatedly
void Exit();
bool Ready();
bool Read(void* buf, size_t size, size_t* got, u64 timeout_ns);
bool Write(const void* buf, size_t size, u64 timeout_ns);
}  // namespace usb_transport

// ---- boot lockscreen auto-clear (lockscreen.cpp) ---------------------------
namespace lockscreen {
void ScheduleUnlock(const AgentConfig& cfg);
}  // namespace lockscreen

// ---- sleep/wake awareness (power_state.cpp) --------------------------------
// Registers with psc:m so the agent is told about sleep/wake transitions
// instead of inferring them from accept() failures after the fact.
namespace power_state {
bool Init(const AgentConfig& cfg);
void Exit();
bool Registered();
bool IsAsleep();
bool TakeWokeFlag();   // consumes the "just woke" edge
}  // namespace power_state

// ---- HDLS virtual controller (shared across input/touch handlers) ----------
// Attaches a virtual Pro Controller on first use; detaches on disconnect so the
// physical controller always regains control.
namespace vpad {
bool Ensure();     // idempotent attach
void Release();    // detach + free work buffer
bool SetButtons(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);
}  // namespace vpad

}  // namespace agent
