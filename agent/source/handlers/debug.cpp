// Live process debugger: attach to a running process and read/write its memory,
// dump its memory map, inspect threads and registers, and search memory.
// Built on Atmosphère's debug syscalls (the same primitives dmnt/EdiZon use).
//
// Model: `debug.attach` pauses the target and holds a debug handle; reads/writes
// operate on the frozen process; `debug.detach` closes the handle and RESUMES
// the target. One debug session at a time.
#include "../protocol.hpp"

#include <switch.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../log.hpp"

namespace agent {
namespace handlers {

namespace {
Handle g_debug = INVALID_HANDLE;
u64 g_pid = 0;

bool Attached(Reply& reply) {
    if (g_debug == INVALID_HANDLE) {
        Fail(reply, "not_attached", "no debug session; call debug.attach first");
        return false;
    }
    return true;
}

const char* MemTypeName(u32 t) {
    switch (t & 0xFF) {
        case MemType_Unmapped: return "unmapped";
        case MemType_Io: return "io";
        case MemType_Normal: return "normal";
        case MemType_CodeStatic: return "code";
        case MemType_CodeMutable: return "code_mutable";
        case MemType_Heap: return "heap";
        case MemType_SharedMem: return "shared";
        case MemType_ModuleCodeStatic: return "module_code";
        case MemType_ModuleCodeMutable: return "module_code_mutable";
    }
    return "other";
}

void PermString(u32 p, char out[4]) {
    out[0] = (p & Perm_R) ? 'r' : '-';
    out[1] = (p & Perm_W) ? 'w' : '-';
    out[2] = (p & Perm_X) ? 'x' : '-';
    out[3] = 0;
}

// svcContinueDebugEvent flags (modern Horizon).
constexpr u32 kContinueExceptionHandled = (1u << 0);
constexpr u32 kContinueAll = (1u << 1);
constexpr u32 kContinueEnableExceptionEvent = (1u << 3);

// Resume all threads of the debugged process (used for non-pausing mode and
// after polling events, so the target keeps running while attached).
void ContinueAll() {
    svcContinueDebugEvent(g_debug,
                          kContinueExceptionHandled | kContinueAll |
                              kContinueEnableExceptionEvent,
                          nullptr, 0);
}

const char* DebugEventName(u32 t) {
    switch (t) {
        case DebugEventType_CreateProcess: return "create_process";
        case DebugEventType_CreateThread: return "create_thread";
        case DebugEventType_ExitProcess: return "exit_process";
        case DebugEventType_ExitThread: return "exit_thread";
        case DebugEventType_Exception: return "exception";
    }
    return "unknown";
}

const char* ExceptionName(u32 t) {
    switch (t) {
        case DebugException_UndefinedInstruction: return "undefined_instruction";
        case DebugException_InstructionAbort: return "instruction_abort";
        case DebugException_DataAbort: return "data_abort";
        case DebugException_AlignmentFault: return "alignment_fault";
        case DebugException_DebuggerAttached: return "debugger_attached";
        case DebugException_BreakPoint: return "breakpoint";
        case DebugException_UserBreak: return "user_break";
        case DebugException_DebuggerBreak: return "debugger_break";
        case DebugException_UndefinedSystemCall: return "undefined_syscall";
        case DebugException_MemorySystemError: return "memory_system_error";
    }
    return "other";
}

// Drain and collect all currently-queued debug events into `arr`.
void DrainEvents(json::Value& arr) {
    DebugEventInfo ev{};
    while (R_SUCCEEDED(svcGetDebugEvent(&ev, g_debug))) {
        json::Value e = json::Value::object();
        e.set("event", DebugEventName(ev.type));
        e.set("thread_id", (int64_t)ev.thread_id);
        if (ev.type == DebugEventType_Exception) {
            e.set("exception", ExceptionName(ev.info.exception.type));
            if (ev.info.exception.type == DebugException_UserBreak) {
                char a[19];
                std::snprintf(a, sizeof(a), "0x%lx",
                              (unsigned long)ev.info.exception.specific.user_break.address);
                e.set("address", a);
            }
        } else if (ev.type == DebugEventType_ExitProcess) {
            e.set("reason", (int64_t)ev.info.exit_process.reason);
        }
        arr.push(std::move(e));
    }
}
}  // namespace

bool DebugAttach(const Request& req, Reply& reply) {
    if (g_debug != INVALID_HANDLE) {
        svcCloseHandle(g_debug);  // replace any existing session
        g_debug = INVALID_HANDLE;
    }
    u64 pid = (u64)req["pid"].as_int(0);
    if (pid == 0) {
        // Default to the foreground application.
        if (R_FAILED(pmdmntGetApplicationProcessId(&pid)) || pid == 0)
            return Fail(reply, "no_app", "no pid given and no foreground app");
    }
    Result rc = svcDebugActiveProcess(&g_debug, pid);
    if (R_FAILED(rc)) {
        g_debug = INVALID_HANDLE;
        return Fail(reply, "attach_failed", "svcDebugActiveProcess failed");
    }
    g_pid = pid;
    // Report the first code region as the main module base (for RVA math).
    u64 base = 0;
    MemoryInfo mi{};
    u32 pageinfo = 0;
    u64 addr = 0;
    for (int i = 0; i < 64 && R_SUCCEEDED(svcQueryDebugProcessMemory(&mi, &pageinfo,
                                                                     g_debug, addr));
         i++) {
        if ((mi.type & 0xFF) == MemType_CodeStatic || (mi.type & 0xFF) == MemType_ModuleCodeStatic) {
            base = mi.addr;
            break;
        }
        if (mi.addr + mi.size <= addr) break;
        addr = mi.addr + mi.size;
    }
    // Non-pausing mode: drain the initial attach events and resume, so the
    // target keeps running while we read/patch memory (live cheat-style work).
    bool pause = req["pause"].as_bool(true);
    if (!pause) {
        json::Value discard = json::Value::array();
        DrainEvents(discard);
        ContinueAll();
    }
    reply.json.set("ok", true);
    reply.json.set("pid", (int64_t)pid);
    char hx[19];
    std::snprintf(hx, sizeof(hx), "0x%lx", (unsigned long)base);
    reply.json.set("main_base", hx);
    reply.json.set("paused", pause);
    reply.json.set("note", pause ? "target paused; detach or debug.continue to resume"
                                  : "target running while attached");
    LOG_INFO("debug attached pid=%lu base=%s pause=%d", (unsigned long)pid, hx, pause);
    return true;
}

// Poll queued debug events (exceptions, thread/proc create/exit, user breaks).
// With a non-pausing attach this is a live log/fault stream — poll repeatedly.
bool DebugPollEvents(const Request& req, Reply& reply) {
    if (!Attached(reply)) return false;
    json::Value events = json::Value::array();
    DrainEvents(events);
    // Resume so execution continues and more events can accrue.
    if (req["resume"].as_bool(true)) ContinueAll();
    reply.json.set("events", std::move(events));
    return true;
}

// Resume a paused target without detaching.
bool DebugContinue(const Request& req, Reply& reply) {
    if (!Attached(reply)) return false;
    (void)req;
    ContinueAll();
    reply.json.set("ok", true);
    return true;
}

// Pause a running attached target (break in).
bool DebugBreak(const Request& req, Reply& reply) {
    if (!Attached(reply)) return false;
    (void)req;
    if (R_FAILED(svcBreakDebugProcess(g_debug)))
        return Fail(reply, "break_failed", "svcBreakDebugProcess failed");
    reply.json.set("ok", true);
    return true;
}

bool DebugDetach(const Request& req, Reply& reply) {
    (void)req;
    if (g_debug != INVALID_HANDLE) {
        svcCloseHandle(g_debug);  // resumes the target
        g_debug = INVALID_HANDLE;
        g_pid = 0;
    }
    reply.json.set("ok", true);
    return true;
}

bool DebugReadMem(const Request& req, Reply& reply) {
    if (!Attached(reply)) return false;
    u64 addr = (u64)req["addr"].as_int(0);
    int64_t len = req["len"].as_int(0x100);
    if (len <= 0 || len > (1 << 20)) len = 1 << 20;
    reply.out.resize(len);
    Result rc = svcReadDebugProcessMemory(reply.out.data(), g_debug, addr, len);
    if (R_FAILED(rc)) return Fail(reply, "read_failed", "svcReadDebugProcessMemory failed");
    reply.json.set("addr", (int64_t)addr);
    return true;
}

bool DebugWriteMem(const Request& req, Reply& reply) {
    if (!Attached(reply)) return false;
    u64 addr = (u64)req["addr"].as_int(0);
    if (req.payload.empty()) return Fail(reply, "bad_arg", "no bytes to write");
    Result rc = svcWriteDebugProcessMemory(g_debug, req.payload.data(), addr,
                                           req.payload.size());
    if (R_FAILED(rc)) return Fail(reply, "write_failed", "svcWriteDebugProcessMemory failed");
    reply.json.set("ok", true);
    reply.json.set("written", (int64_t)req.payload.size());
    return true;
}

bool DebugMemMap(const Request& req, Reply& reply) {
    if (!Attached(reply)) return false;
    (void)req;
    json::Value regions = json::Value::array();
    u64 addr = 0;
    for (int i = 0; i < 512; i++) {
        MemoryInfo mi{};
        u32 pageinfo = 0;
        if (R_FAILED(svcQueryDebugProcessMemory(&mi, &pageinfo, g_debug, addr))) break;
        if ((mi.type & 0xFF) != MemType_Unmapped) {
            json::Value r = json::Value::object();
            char a[19], s[19];
            std::snprintf(a, sizeof(a), "0x%lx", (unsigned long)mi.addr);
            std::snprintf(s, sizeof(s), "0x%lx", (unsigned long)mi.size);
            char perm[4];
            PermString(mi.perm, perm);
            r.set("addr", a);
            r.set("size", s);
            r.set("type", MemTypeName(mi.type));
            r.set("perm", perm);
            regions.push(std::move(r));
        }
        u64 next = mi.addr + mi.size;
        if (next <= addr) break;  // wrapped / done
        addr = next;
    }
    reply.json.set("regions", std::move(regions));
    return true;
}

bool DebugThreads(const Request& req, Reply& reply) {
    if (!Attached(reply)) return false;
    (void)req;
    u64 tids[128];
    s32 count = 0;
    if (R_FAILED(svcGetThreadList(&count, tids, 128, g_debug)))
        return Fail(reply, "svc_failed", "svcGetThreadList failed");
    json::Value list = json::Value::array();
    for (s32 i = 0; i < count; i++) list.push((int64_t)tids[i]);
    reply.json.set("threads", std::move(list));
    return true;
}

bool DebugRegisters(const Request& req, Reply& reply) {
    if (!Attached(reply)) return false;
    u64 tid = (u64)req["thread_id"].as_int(0);
    if (tid == 0) {
        // Default to the first thread.
        u64 tids[8];
        s32 count = 0;
        if (R_FAILED(svcGetThreadList(&count, tids, 8, g_debug)) || count == 0)
            return Fail(reply, "no_threads", "no threads to inspect");
        tid = tids[0];
    }
    ThreadContext ctx{};
    if (R_FAILED(svcGetDebugThreadContext(&ctx, g_debug, tid, 0xF)))
        return Fail(reply, "ctx_failed", "svcGetDebugThreadContext failed");
    json::Value regs = json::Value::object();
    char buf[19];
    for (int i = 0; i < 29; i++) {
        char key[8];
        std::snprintf(key, sizeof(key), "x%d", i);
        std::snprintf(buf, sizeof(buf), "0x%lx", (unsigned long)ctx.cpu_gprs[i].x);
        regs.set(key, buf);
    }
    std::snprintf(buf, sizeof(buf), "0x%lx", (unsigned long)ctx.fp); regs.set("fp", buf);
    std::snprintf(buf, sizeof(buf), "0x%lx", (unsigned long)ctx.lr); regs.set("lr", buf);
    std::snprintf(buf, sizeof(buf), "0x%lx", (unsigned long)ctx.sp); regs.set("sp", buf);
    std::snprintf(buf, sizeof(buf), "0x%lx", (unsigned long)ctx.pc.x); regs.set("pc", buf);
    reply.json.set("thread_id", (int64_t)tid);
    reply.json.set("registers", std::move(regs));
    return true;
}

// Search writable/heap regions for a little-endian integer value (1/2/4/8
// bytes). Returns up to `max` matching addresses — the core of cheat/mod dev.
bool DebugSearch(const Request& req, Reply& reply) {
    if (!Attached(reply)) return false;
    u64 value = (u64)req["value"].as_int(0);
    int width = (int)req["width"].as_int(4);
    if (width != 1 && width != 2 && width != 4 && width != 8) width = 4;
    int64_t max = req["max"].as_int(64);
    if (max <= 0 || max > 1024) max = 64;

    uint8_t needle[8] = {0};
    std::memcpy(needle, &value, width);

    json::Value matches = json::Value::array();
    int found = 0;
    std::vector<uint8_t> chunk;
    u64 addr = 0;
    for (int i = 0; i < 512 && found < max; i++) {
        MemoryInfo mi{};
        u32 pageinfo = 0;
        if (R_FAILED(svcQueryDebugProcessMemory(&mi, &pageinfo, g_debug, addr))) break;
        u64 next = mi.addr + mi.size;
        bool scannable = (mi.perm & Perm_W) && (mi.type & 0xFF) != MemType_Unmapped;
        // Cap per-region scan to keep memory/time bounded.
        if (scannable && mi.size <= (64u << 20)) {
            chunk.resize(mi.size);
            if (R_SUCCEEDED(svcReadDebugProcessMemory(chunk.data(), g_debug, mi.addr, mi.size))) {
                for (u64 off = 0; off + width <= mi.size && found < max; off++) {
                    if (std::memcmp(chunk.data() + off, needle, width) == 0) {
                        char a[19];
                        std::snprintf(a, sizeof(a), "0x%lx", (unsigned long)(mi.addr + off));
                        matches.push(std::string(a));
                        found++;
                    }
                }
            }
        }
        if (next <= addr) break;
        addr = next;
    }
    reply.json.set("matches", std::move(matches));
    reply.json.set("count", (int64_t)found);
    reply.json.set("truncated", found >= max);
    return true;
}

// --- hardware breakpoints and watchpoints ------------------------------------
//
// The AArch64 debug architecture gives each core a small bank of breakpoint
// registers (DBGBVR/DBGBCR, instruction) and watchpoint registers
// (DBGWVR/DBGWCR, data). Horizon exposes them through one syscall:
//
//   svcSetHardwareBreakPoint(which, flags, value)
//     which — register selector: 0..15 are instruction breakpoints,
//             16..31 are data watchpoints
//     flags — the DBGBCR/DBGWCR control word; bit 0 is the enable bit, and
//             clearing the whole word disables that slot
//     value — DBGBVR/DBGWVR, the address being watched
//
// A watchpoint is the reason this exists: "stop when *this address* is
// written" turns a needle-in-4GB memory hunt into a single hit, which polling
// reads can never do. The cost is that only a handful of slots exist in
// hardware (typically 6 breakpoints, 4 watchpoints on this SoC), so slots are
// addressed explicitly rather than allocated behind the caller's back.
//
// Hits surface as exceptions through the existing debug event stream, so pair
// these with debug.poll_events.

namespace {

constexpr u32 kWatchpointBase = 16;  // `which` offset for data watchpoints

// Slot counts measured on this SoC (Cortex-A57): svcSetHardwareBreakPoint
// accepts I0-I5 and D0-D3 and rejects the rest, matching the 6 breakpoint /
// 4 watchpoint debug registers the core implements.
constexpr int kMaxBreakpointSlots = 6;
constexpr int kMaxWatchpointSlots = 4;

// DBGBCR for an instruction breakpoint.
//   bit 0      E    enable
//   bits 1-2   PMC  privilege — MUST be 0. Horizon programs the privilege
//                   field itself and rejects any caller-supplied value with
//                   InvalidCombination (kernel desc 116).
//   bits 5-8   BAS  byte address select: 0b1111 for a full A64 instruction
//   bits 20-23 BT   breakpoint type — MUST be 0b0001 (linked address match).
//                   The kernel links the breakpoint to the debugged process's
//                   context itself; unlinked (0b0000) is refused, and the
//                   ContextIdr register is not exposed to userspace at all
//                   (which=32 returns InvalidEnumValue).
// Verified empirically against firmware 22.1.0 by sweeping the encoding space.
constexpr u64 kBreakpointCtl = 1u | (0b1111u << 5) | (0b0001ull << 20);

// DBGWCR for an EL0 data watchpoint. LSC (bits 3-4) selects the access type:
// 0b01 load, 0b10 store, 0b11 either. BAS (bits 5-12) selects which bytes
// within the aligned doubleword are covered.
// Surface the raw kernel Result. "failed" is useless here: the difference
// between "syscall not permitted by the npdm" (2168-0006 style) and "the
// control-word encoding was rejected" decides whether the fix is a manifest
// change or a bit-layout change.
bool FailResult(Reply& reply, Result rc, const char* what, u64 ctl, u64 value) {
    char msg[256];
    std::snprintf(msg, sizeof(msg),
                  "svcSetHardwareBreakPoint(%s) failed: rc=0x%08x "
                  "(module=%u desc=%u) ctl=0x%llx value=0x%llx",
                  what, (unsigned)rc, (unsigned)(rc & 0x1FF),
                  (unsigned)((rc >> 9) & 0x1FFF), (unsigned long long)ctl,
                  (unsigned long long)value);
    return Fail(reply, "hw_bp_failed", msg);
}

// DBGWCR for a data watchpoint.
//   bit 0      E    enable
//   bits 1-2   PAC  privilege — MUST be 0. Horizon programs the privilege field
//                   itself and rejects a caller-supplied value with
//                   InvalidCombination, exactly as for PMC on breakpoints.
//   bits 3-4   LSC  access type: 0b01 load, 0b10 store, 0b11 either
//   bits 5-12  BAS  which bytes of the aligned doubleword are covered
u64 WatchpointCtl(u32 lsc, u32 bas) {
    return 1u | ((u64)(lsc & 0b11) << 3) | ((u64)(bas & 0xFF) << 5);
}

}  // namespace

// Raw passthrough to svcSetHardwareBreakPoint.
//
// Horizon validates the DBGBCR/DBGWCR control word strictly and rejects
// anything it does not like with InvalidCombination (kernel desc 116), without
// saying which field was wrong. Rebuilding and rebooting per guess costs a
// minute each, so this exposes `which`/`flags`/`value` directly and lets the
// encoding be swept from the client in seconds. Research tool: the typed
// set_watchpoint/set_breakpoint commands above are the supported interface.
bool DebugSetHwBpRaw(const Request& req, Reply& reply) {
    if (!Attached(reply)) return false;
    u32 which = (u32)req["which"].as_int(0);
    u64 flags = (u64)req["flags"].as_int(0);
    u64 value = (u64)req["value"].as_int(0);
    Result rc = svcSetHardwareBreakPoint(which, flags, value);
    reply.json.set("ok", R_SUCCEEDED(rc));
    reply.json.set("rc", (int64_t)rc);
    reply.json.set("module", (int64_t)(rc & 0x1FF));
    reply.json.set("desc", (int64_t)((rc >> 9) & 0x1FFF));
    reply.json.set("which", (int64_t)which);
    return true;   // report the code rather than failing: sweeps want data
}

// Set a hardware watchpoint: break when `addr` is read and/or written.
bool DebugSetWatchpoint(const Request& req, Reply& reply) {
    if (!Attached(reply)) return false;

    u64 addr = (u64)req["addr"].as_int(0);
    int size = (int)req["size"].as_int(4);
    int slot = (int)req["slot"].as_int(0);
    std::string mode = req["mode"].as_string("write");

    if (!addr) return Fail(reply, "bad_arg", "missing addr");
    if (size != 1 && size != 2 && size != 4 && size != 8)
        return Fail(reply, "bad_arg", "size must be 1, 2, 4 or 8 bytes");
    if (slot < 0 || slot >= kMaxWatchpointSlots)
        return Fail(reply, "bad_arg", "slot must be 0-3 (this SoC has 4 watchpoints)");

    // A watchpoint covers bytes within one aligned doubleword; BAS says which.
    // Straddling that boundary needs two watchpoints, so reject it explicitly
    // rather than silently watching only half the range.
    u64 base = addr & ~7ULL;
    u32 offset = (u32)(addr - base);
    if (offset + (u32)size > 8)
        return Fail(reply, "bad_arg",
                    "range crosses an 8-byte boundary; align it or use two slots");
    u32 bas = (u32)(((1u << size) - 1) << offset);

    u32 lsc;
    if (mode == "read") lsc = 0b01;
    else if (mode == "write") lsc = 0b10;
    else if (mode == "rw" || mode == "both") lsc = 0b11;
    else return Fail(reply, "bad_arg", "mode must be read, write or rw");

    u64 ctl = req.msg.has("ctl") ? (u64)req["ctl"].as_int(0)
                                  : WatchpointCtl(lsc, bas);
    Result rc = svcSetHardwareBreakPoint(kWatchpointBase + (u32)slot, ctl, base);
    if (R_FAILED(rc)) return FailResult(reply, rc, "watchpoint", ctl, base);

    LOG_INFO("watchpoint slot %d: %s at 0x%lx (%d bytes)", slot, mode.c_str(),
             (unsigned long)addr, size);
    reply.json.set("ok", true);
    reply.json.set("slot", (int64_t)slot);
    reply.json.set("mode", mode);
    reply.json.set("watching", (int64_t)addr);
    reply.json.set("note",
                   "armed, but DO NOT RELY ON HITS. On fw 22.1.0 no watchpoint "
                   "has ever been observed to fire from this sysmodule. The "
                   "syscall is permitted and the encoding is accepted; the "
                   "exception is simply never delivered. Core affinity was "
                   "tested and ruled out. Use cheat.read_mem polling or the "
                   "narrowing search (search.begin/next) instead.");
    return true;
}

// Set a hardware instruction breakpoint: break when `addr` is executed.
bool DebugSetBreakpoint(const Request& req, Reply& reply) {
    if (!Attached(reply)) return false;

    u64 addr = (u64)req["addr"].as_int(0);
    int slot = (int)req["slot"].as_int(0);
    if (!addr) return Fail(reply, "bad_arg", "missing addr");
    if (addr & 3ULL)
        return Fail(reply, "bad_arg", "address must be 4-byte aligned (A64 instruction)");
    if (slot < 0 || slot >= kMaxBreakpointSlots)
        return Fail(reply, "bad_arg", "slot must be 0-5 (this SoC has 6 breakpoints)");

    Result rc = svcSetHardwareBreakPoint((u32)slot, kBreakpointCtl, addr);
    if (R_FAILED(rc)) return FailResult(reply, rc, "breakpoint", kBreakpointCtl, addr);

    LOG_INFO("breakpoint slot %d at 0x%lx", slot, (unsigned long)addr);
    reply.json.set("ok", true);
    reply.json.set("slot", (int64_t)slot);
    reply.json.set("breaking_at", (int64_t)addr);
    reply.json.set("note",
                   "armed, but DO NOT RELY ON HITS. No hardware breakpoint has "
                   "been observed to fire on fw 22.1.0 from this sysmodule. "
                   "Arming and clearing work; the exception is never delivered. "
                   "Core affinity was tested and ruled out.");
    return true;
}

// Free a slot. Slots are a scarce hardware resource, so leaving them set after
// you are done silently costs you the next watchpoint.
bool DebugClearBreakpoint(const Request& req, Reply& reply) {
    if (!Attached(reply)) return false;
    int slot = (int)req["slot"].as_int(0);
    bool is_watchpoint = req["watchpoint"].as_bool(false);
    int limit = is_watchpoint ? kMaxWatchpointSlots : kMaxBreakpointSlots;
    if (slot < 0 || slot >= limit)
        return Fail(reply, "bad_arg",
                    is_watchpoint ? "watchpoint slot must be 0-3"
                                  : "breakpoint slot must be 0-5");

    u32 which = (is_watchpoint ? kWatchpointBase : 0u) + (u32)slot;
    // Control word 0 clears the enable bit and the whole configuration.
    if (R_FAILED(svcSetHardwareBreakPoint(which, 0, 0)))
        return Fail(reply, "hw_bp_failed", "could not clear the slot");

    reply.json.set("ok", true);
    reply.json.set("cleared_slot", (int64_t)slot);
    reply.json.set("kind", is_watchpoint ? "watchpoint" : "breakpoint");
    return true;
}

// Write CPU registers of a stopped thread. svcSetDebugThreadContext was already
// permitted by the npdm but unreachable — so registers could be read and never
// changed, which rules out redirecting execution, skipping a faulting
// instruction, or forcing a return value.
bool DebugWriteRegisters(const Request& req, Reply& reply) {
    if (!Attached(reply)) return false;

    u64 tid = (u64)req["thread_id"].as_int(0);
    if (tid == 0) {
        u64 tids[8];
        s32 count = 0;
        if (R_FAILED(svcGetThreadList(&count, tids, 8, g_debug)) || count == 0)
            return Fail(reply, "no_threads", "no threads to modify");
        tid = tids[0];
    }

    // Read the current context first and patch only the named registers, so a
    // caller setting just `pc` does not zero everything else.
    ThreadContext ctx{};
    if (R_FAILED(svcGetDebugThreadContext(&ctx, g_debug, tid, 0xF)))
        return Fail(reply, "ctx_failed", "svcGetDebugThreadContext failed");

    const json::Value& regs = req["registers"];
    if (!regs.is_object())
        return Fail(reply, "bad_arg", "expected a 'registers' object, e.g. {\"pc\": 123}");

    // Write only the register groups we actually touched. This is the correct
    // thing to do regardless (do not rewrite PSTATE when the caller only asked
    // to change x0), but note it is NOT why this call fails on most targets:
    // sweeping every mask from 0 to 15 on a stopped thread produced the same
    // error every time. See the error text below for what is actually known.
    constexpr u32 kGroupCpuGprs = 1;  // x0-x28, fp (x29), lr (x30)
    constexpr u32 kGroupCpuSprs = 2;  // sp, pc, pstate

    int changed = 0;
    u32 groups = 0;
    for (int i = 0; i < 29; i++) {
        char key[8];
        std::snprintf(key, sizeof(key), "x%d", i);
        if (regs.has(key)) {
            ctx.cpu_gprs[i].x = (u64)regs[key].as_int(0);
            groups |= kGroupCpuGprs;
            changed++;
        }
    }
    if (regs.has("fp")) { ctx.fp = (u64)regs["fp"].as_int(0); groups |= kGroupCpuGprs; changed++; }
    if (regs.has("lr")) { ctx.lr = (u64)regs["lr"].as_int(0); groups |= kGroupCpuGprs; changed++; }
    if (regs.has("sp")) { ctx.sp = (u64)regs["sp"].as_int(0); groups |= kGroupCpuSprs; changed++; }
    if (regs.has("pc")) { ctx.pc.x = (u64)regs["pc"].as_int(0); groups |= kGroupCpuSprs; changed++; }

    if (changed == 0)
        return Fail(reply, "bad_arg", "no recognised register names in 'registers'");

    // Escape hatch for experimenting against a kernel that disagrees.
    if (req.msg.has("groups")) groups = (u32)req["groups"].as_int(groups);

    reply.json.set("groups", (int64_t)groups);
    Result ctx_rc = svcSetDebugThreadContext(g_debug, tid, &ctx, groups);
    if (R_FAILED(ctx_rc)) {
        char m[224];
        std::snprintf(m, sizeof(m),
                      "svcSetDebugThreadContext failed: rc=0x%08x (module=%u "
                      "desc=%u) thread=%llu groups=0x%x. desc=125 is kernel "
                      "InvalidState: Horizon only allows a thread's context to "
                      "be written while that thread is stopped AT A DEBUG EVENT "
                      "(an exception, breakpoint or fault reported through "
                      "debug.poll_events) - a paused attach alone is not "
                      "enough. Get the thread to trap first, then write.",
                      (unsigned)ctx_rc, (unsigned)(ctx_rc & 0x1FF),
                      (unsigned)((ctx_rc >> 9) & 0x1FFF),
                      (unsigned long long)tid, (unsigned)groups);
        return Fail(reply, "ctx_failed", m);
    }
    if (false)
        return Fail(reply, "ctx_failed",
                    "svcSetDebugThreadContext failed. Known-flaky on fw 22.1.0: "
                    "this has succeeded exactly once (nx-hbmenu) and fails on "
                    "system processes, sysmodules and running games alike, on "
                    "every thread, with or without a prior debug.break. Root "
                    "cause not yet identified. Register reads are unaffected.");

    LOG_WARN("wrote %d register(s) on thread %lu", changed, (unsigned long)tid);
    reply.json.set("ok", true);
    reply.json.set("thread_id", (int64_t)tid);
    reply.json.set("registers_written", (int64_t)changed);
    return true;
}


// List the loaded modules of a process with their base, size and build ID.
//
// Without this every address is a bare number. With it an address becomes
// "main+0x1a2f4", the form Ghidra and IDA speak, and the build ID is the key
// that matches a process to a symbol map or an existing RE database. Works on
// any process, attached or not - it goes through ldr:dmnt, not the debug
// session.
bool DebugModules(const Request& req, Reply& reply) {
    u64 pid = (u64)req["pid"].as_int(0);
    if (pid == 0) {
        if (g_debug != INVALID_HANDLE) {
            pid = g_pid;                       // default to the attached process
        } else if (R_FAILED(pmdmntGetApplicationProcessId(&pid)) || pid == 0) {
            return Fail(reply, "no_pid", "no pid given, none attached, no foreground app");
        }
    }

    if (R_FAILED(ldrDmntInitialize()))
        return Fail(reply, "ldr_failed", "cannot open ldr:dmnt");

    LoaderModuleInfo infos[16];
    s32 count = 0;
    Result rc = ldrDmntGetProcessModuleInfo(pid, infos, 16, &count);
    ldrDmntExit();
    if (R_FAILED(rc))
        return Fail(reply, "ldr_failed", "ldrDmntGetProcessModuleInfo failed");

    json::Value arr = json::Value::array();
    for (s32 i = 0; i < count; i++) {
        json::Value m = json::Value::object();
        char hx[19];
        std::snprintf(hx, sizeof(hx), "0x%lx", (unsigned long)infos[i].base_address);
        m.set("base", hx);
        std::snprintf(hx, sizeof(hx), "0x%lx", (unsigned long)infos[i].size);
        m.set("size", hx);
        // Build ID identifies this exact binary across runs and reboots.
        char bid[0x41];
        for (int b = 0; b < 0x20; b++)
            std::snprintf(bid + b * 2, 3, "%02x", infos[i].build_id[b]);
        m.set("build_id", bid);
        m.set("index", (int64_t)i);
        arr.push(std::move(m));
    }
    reply.json.set("pid", (int64_t)pid);
    reply.json.set("modules", std::move(arr));
    return true;
}

// Walk the frame-pointer chain of a stopped thread to produce a backtrace.
//
// AArch64 frames link through x29: [fp] is the caller's fp and [fp+8] is its
// return address. Turning a fault into a call chain is the difference between
// "it crashed somewhere" and "it crashed down this path".
bool DebugBacktrace(const Request& req, Reply& reply) {
    if (!Attached(reply)) return false;

    u64 tid = (u64)req["thread_id"].as_int(0);
    if (tid == 0) {
        u64 tids[8];
        s32 n = 0;
        if (R_FAILED(svcGetThreadList(&n, tids, 8, g_debug)) || n == 0)
            return Fail(reply, "no_threads", "no threads to walk");
        tid = tids[0];
    }
    int max_frames = (int)req["max_frames"].as_int(32);
    if (max_frames < 1 || max_frames > 64) max_frames = 32;

    ThreadContext ctx{};
    if (R_FAILED(svcGetDebugThreadContext(&ctx, g_debug, tid, 0xF)))
        return Fail(reply, "ctx_failed", "cannot read thread context");

    json::Value frames = json::Value::array();
    char hx[19];

    json::Value f0 = json::Value::object();
    std::snprintf(hx, sizeof(hx), "0x%lx", (unsigned long)ctx.pc.x);
    f0.set("pc", hx);
    std::snprintf(hx, sizeof(hx), "0x%lx", (unsigned long)ctx.lr);
    f0.set("lr", hx);
    frames.push(std::move(f0));

    u64 fp = ctx.fp;
    u64 prev_fp = 0;
    for (int i = 1; i < max_frames && fp; i++) {
        // A frame pointer must climb and stay 8-byte aligned; anything else
        // means we have walked off a real chain into garbage.
        if (fp <= prev_fp || (fp & 7)) break;
        u64 pair[2] = {0, 0};
        if (R_FAILED(svcReadDebugProcessMemory(pair, g_debug, fp, sizeof(pair)))) break;
        u64 next_fp = pair[0], ret = pair[1];
        if (!ret) break;
        json::Value f = json::Value::object();
        std::snprintf(hx, sizeof(hx), "0x%lx", (unsigned long)ret);
        f.set("pc", hx);
        std::snprintf(hx, sizeof(hx), "0x%lx", (unsigned long)fp);
        f.set("fp", hx);
        frames.push(std::move(f));
        prev_fp = fp;
        fp = next_fp;
    }

    reply.json.set("thread_id", (int64_t)tid);
    reply.json.set("frames", std::move(frames));
    reply.json.set("note",
                   "addresses are absolute; subtract a module base from "
                   "debug.modules to get an RVA for Ghidra/IDA");
    return true;
}

}  // namespace handlers
}  // namespace agent
