// switch-agentd — background remote-management service for Atmosphère.
// Speaks the protocol documented in the wiki, on TCP port 6060.

#include <switch.h>

#include <cstdio>
#include <cstring>

#include "protocol.hpp"

// Sysmodules get a fixed heap; keep it small. 4 MiB is enough for the
// 1 MiB transfer chunk + JPEG screenshot buffer + slack.
extern "C" {
u32 __nx_applet_type = AppletType_None;
#define INNER_HEAP_SIZE (4 * 1024 * 1024)
size_t nx_inner_heap_size = INNER_HEAP_SIZE;
char nx_inner_heap[INNER_HEAP_SIZE];
}  // extern "C" (reopened below)

// --- Memory budget audit (Phase 9) ------------------------------------------
// Only one request is in flight per connection, so peak working set is the
// worst single handler, not their sum. Largest contributors (all heap):
//   fs.write : inbound payload ...................... ≤ 1 MiB (kMaxBinary)
//   fs.read  : outbound reply.out ................... ≤ 1 MiB (kMaxBinary)
//   screenshot: JPEG capture buffer ................. 512 KiB
//   titles   : NsApplicationControlData ............. 128 KiB
//   frame    : JSON text (in + dumped out) .......... ≤ 128 KiB (2×kMaxJson)
// A single request touches at most one of the big buffers plus frame JSON, so
// realistic peak ≈ 1 MiB payload + 128 KiB JSON + allocator slack. We budget a
// conservative 2.5 MiB ceiling and assert it fits the heap with margin. If a
// future handler allocates more, this fails at compile time.
namespace {
// The narrowing memory search keeps one (address, last_value) pair per
// candidate on the heap between scans.
//
// This budget is SMALL on purpose. Raising INNER_HEAP_SIZE to 16 MiB to allow a
// bigger table stopped the sysmodule booting at all: a sysmodule's heap comes
// from a constrained system pool, the allocation failed during init, and the
// module died before it could log a line or produce a crash report — which
// looks exactly like a brick from the outside and needed the SD card pulled to
// diagnose. 4 MiB is the proven-good size; the table lives within it.
//
// 32k candidates is enough to converge: the point of narrowing search is that
// the first scan is followed immediately by a filter, and a first scan that
// overflows this is a sign the starting value was too common to be useful.
constexpr size_t kMemSearchBudget = 32768 * 16;   // 512 KiB

constexpr size_t kWorstCaseHeap =
    (1u << 20)          /* inbound or outbound 1 MiB transfer payload */
    + (1u << 20)        /* a second large buffer, e.g. an on-device copy */
    + (512u << 10)      /* screenshot JPEG */
    + (128u << 10)      /* NsApplicationControlData */
    + kMemSearchBudget; /* narrowing-search candidate table */
static_assert(kWorstCaseHeap < INNER_HEAP_SIZE - (512u << 10),
              "agent heap budget exceeded; raise INNER_HEAP_SIZE or reduce caps");

// Hard ceiling on the heap itself. A sysmodule's heap comes from a constrained
// system pool, not from free RAM: raising this to 16 MiB made the module fail
// during __libnx_initheap and die before it could log anything or produce a
// crash report. From the outside that is indistinguishable from a brick, and
// diagnosing it meant physically removing the SD card.
//
// So do not "just raise it" to make something fit. 4 MiB is proven on this
// firmware. If more room is genuinely needed, the right move is a system
// resource size in switch-agentd.json plus testing with the SD card to hand — not
// a bigger number here.
static_assert(INNER_HEAP_SIZE <= 4 * 1024 * 1024,
              "INNER_HEAP_SIZE above 4 MiB has been observed to stop the "
              "sysmodule booting; see the comment above before changing this");
}  // namespace

extern "C" {

void __libnx_initheap(void) {
    extern char* fake_heap_start;
    extern char* fake_heap_end;
    fake_heap_start = nx_inner_heap;
    fake_heap_end = nx_inner_heap + nx_inner_heap_size;
}

void __appInit(void) {
    Result rc = smInitialize();
    if (R_FAILED(rc)) fatalThrow(rc);

    // Wait for SD + settings so we can read config.ini.
    rc = setsysInitialize();
    if (R_SUCCEEDED(rc)) {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw)))
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
    }

    if (R_FAILED(rc = fsInitialize())) fatalThrow(rc);
    fsdevMountSdmc();

    // Services backing the tools. Optional ones are allowed to fail — the
    // corresponding handler will report an error rather than the module dying.
    socketInitializeDefault();  // TCP listener
    capsscInitialize();         // screenshots (capssc)
    psmInitialize();            // battery
    tsInitialize();             // temperature sensors
    nifmInitialize(NifmServiceType_System);  // network status
    nsInitialize();             // installed-title listing / control data
    pmshellInitialize();        // launch/terminate titles
    pmdmntInitialize();         // foreground app pid
    pminfoInitialize();         // pid -> program id
    hiddbgInitialize();         // virtual controller (HDLS) + touch
    bpcInitialize();            // reboot / shutdown
    lblInitialize();            // backlight / brightness
    pcvInitialize();            // clock control (gated by config)
    clkrstInitialize();         // clock rate reads (modern fw)
    audctlInitialize();         // system volume
    timeInitialize();           // get/set system time
    setInitialize();            // region code (set service)
    capsaInitialize();          // capture album access
    i2cInitialize();            // low-level i2c bus (gated)
    gpioInitialize();           // low-level gpio (gated)
    hidInitialize();            // controller connection/power info
    hidSetSupportedNpadStyleSet(HidNpadStyleSet_NpadStandard);
    {
        static const HidNpadIdType kIds[] = {
            HidNpadIdType_No1, HidNpadIdType_No2, HidNpadIdType_No3, HidNpadIdType_No4,
            HidNpadIdType_No5, HidNpadIdType_No6, HidNpadIdType_No7, HidNpadIdType_No8,
            HidNpadIdType_Handheld};
        hidSetSupportedNpadIdType(kIds, sizeof(kIds) / sizeof(kIds[0]));
    }

    // Keep the sm session open: some services (e.g. dmnt:cht for the cheat
    // engine) are opened lazily at runtime via smGetService and need it.
}

void __appExit(void) {
    agent::power_state::Exit();
    clkrstExit();
    hidExit();
    gpioExit();
    i2cExit();
    capsaExit();
    setExit();
    timeExit();
    audctlExit();
    pcvExit();
    lblExit();
    bpcExit();
    hiddbgExit();
    pminfoExit();
    pmdmntExit();
    pmshellExit();
    nsExit();
    nifmExit();
    tsExit();
    psmExit();
    capsscExit();
    socketExit();
    fsdevUnmountAll();
    fsExit();
    setsysExit();
}
}  // extern "C"

int main(int argc, char* argv[]) {
    AgentConfig cfg = agent::LoadConfig("sdmc:/config/switch-agentd/config.ini");
    // psc now uses a dedicated acknowledgement thread (see power_state.cpp).
    // It stays opt-in via enable_psc: a registered module that fails to answer
    // a sleep request hangs the console, and the fallback path cannot.
    agent::handlers::WatchdogOnBoot();   // counts boots with no client yet
    agent::power_state::Init(cfg);
    // Fire-and-forget: clears the boot lockscreen a few seconds from now on its
    // own thread, so the listener below comes up immediately.
    agent::lockscreen::ScheduleUnlock(cfg);

    agent::Server server(cfg);
    server.Run();  // accept loop: one client at a time, frame-dispatch
    return 0;
}
