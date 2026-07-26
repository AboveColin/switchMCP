// Clear the boot lockscreen so the console stays reachable.
//
// The console boots to the lockscreen, and the lockscreen puts itself to sleep
// even when auto-sleep is set to Never in Settings. Sleeping tears down the
// network stack, so a console left at the lockscreen after a reboot silently
// disappears and only a physical power-cycle brings it back.
//
// Doing this from the client is a race that the client loses: it has to wait
// for Wi-Fi to associate, reconnect, and handshake before it can send anything,
// which took 20-40 s in practice — comfortably longer than the lockscreen
// takes to give up. The agent is already running seconds after boot, so it
// clears the lockscreen itself and there is no race to lose.
//
// B, not A: B unlocks the lockscreen just as well, but is "back/cancel"
// everywhere else. If the console is already past the lockscreen (a fast
// reboot, or the user got there first) an A press lands on the home menu and
// launches whatever is selected.
#include "protocol.hpp"

#include <switch.h>

#include "log.hpp"

namespace agent {
namespace lockscreen {

namespace {

// Presses are spread over a window rather than fired at one instant: the exact
// moment the UI becomes able to accept input varies with boot timing, and a
// single well-timed guess is not reliable.
constexpr u64 kFirstPressNs = 12'000'000'000ULL;  // 12 s: UI is usually up
constexpr u64 kGapNs = 4'000'000'000ULL;          // 4 s between presses
constexpr int kPresses = 3;

Thread g_thread;
bool g_started = false;

void PressB() {
    if (!vpad::Ensure()) return;
    vpad::SetButtons(HidNpadButton_B, 0, 0, 0, 0);
    svcSleepThread(120'000'000ULL);  // 120 ms hold
    vpad::SetButtons(0, 0, 0, 0, 0);
}

// True when a game/application is in the foreground. Used to make absolutely
// sure the keep-awake nudge never lands in someone's game.
bool ApplicationRunning() {
    u64 pid = 0;
    return R_SUCCEEDED(pmdmntGetApplicationProcessId(&pid)) && pid != 0;
}

u64 g_keep_awake_ns = 0;

void Worker(void*) {
    svcSleepThread(kFirstPressNs);
    for (int i = 0; i < kPresses; i++) {
        PressB();
        LOG_INFO("lockscreen: sent B press %d/%d", i + 1, kPresses);
        if (i + 1 < kPresses) svcSleepThread(kGapNs);
    }
    vpad::Release();
    LOG_INFO("lockscreen: done, virtual pad released");

    if (!g_keep_awake_ns) return;

    // Keep-awake. Auto-sleep set to Never still leaves the console able to
    // re-lock when idle, and the lockscreen sleeps on its own — which drops the
    // network and strands a headless console. A periodic button press keeps the
    // idle timer from ever reaching that point.
    //
    // Skipped whenever an application is running: a stray press in a game is
    // exactly the kind of surprise that makes a tool untrustworthy. With no game
    // running the press lands on the home menu, where B does nothing.
    LOG_INFO("keep-awake: nudging every %llus while no game is running",
             (unsigned long long)(g_keep_awake_ns / 1'000'000'000ULL));
    while (true) {
        svcSleepThread(g_keep_awake_ns);
        if (ApplicationRunning()) {
            LOG_DEBUG("keep-awake: application running, skipping nudge");
            continue;
        }
        PressB();
        vpad::Release();
        LOG_DEBUG("keep-awake: nudged");
    }
}

}  // namespace

// Kick off the boot-time unlock on its own thread. Returns immediately so the
// listener comes up without waiting.
void ScheduleUnlock(const AgentConfig& cfg) {
    if (g_started) return;
    g_keep_awake_ns = (u64)cfg.keep_awake_minutes * 60ULL * 1'000'000'000ULL;
    if (!cfg.clear_lockscreen_on_boot && !cfg.keep_awake_minutes) {
        LOG_INFO("lockscreen: auto-clear and keep-awake both disabled");
        return;
    }
    // Injecting input is a Control-tier action; an observe-only agent must not
    // touch the console at all.
    if ((int)cfg.tier < (int)Tier::Control) {
        LOG_INFO("lockscreen: auto-clear needs tier>=control; skipping");
        return;
    }
    if (R_FAILED(threadCreate(&g_thread, Worker, nullptr, nullptr, 0x2000, 49, 3))) {
        LOG_WARN("lockscreen: could not create unlock thread");
        return;
    }
    if (R_FAILED(threadStart(&g_thread))) {
        threadClose(&g_thread);
        LOG_WARN("lockscreen: could not start unlock thread");
        return;
    }
    g_started = true;
    LOG_INFO("lockscreen: will clear in %llus", (unsigned long long)(kFirstPressNs / 1'000'000'000ULL));
}

}  // namespace lockscreen
}  // namespace agent
