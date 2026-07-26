// Sleep/wake awareness via psc:m.
//
// The console tears down the network stack when it sleeps. Without a
// notification the agent only discovers this by way of accept() failing
// repeatedly, and the listener-rebuild heuristic in Server::Run needs ~20
// failures (~2 s) plus a rebuild before it recovers — and until then a client
// sees a dead port with no explanation.
//
// psc:m lets a module register as a power-state participant: the system asks
// every registered module to acknowledge a transition before it happens. That
// gives us a clean edge on both sides — tear the listener down deliberately
// before sleep, rebuild it the moment we are awake — instead of inferring the
// transition from errno afterwards.
//
// Note this does NOT keep the console awake. A psc module can only observe and
// acknowledge; refusing to acknowledge stalls the transition rather than
// cancelling it, which would be a good way to hang the console, so we always
// acknowledge promptly.
#include "protocol.hpp"

#include <switch.h>

#include "log.hpp"

namespace agent {
namespace power_state {

namespace {

PscPmModule g_module{};
Thread g_thread;
bool g_registered = false;
volatile bool g_stop = false;
volatile bool g_woke = false;

// Any unused module id works for a third-party sysmodule; this range is not
// claimed by the system modules psc knows about.
constexpr PscPmModuleId kModuleId = (PscPmModuleId)0xAE57;

volatile bool g_asleep = false;

}  // namespace

// The acknowledgement thread. Its ONLY job is to answer the system promptly.
//
// This is the entire lesson of the first attempt: acknowledgement was polled
// from the accept() error path, which never runs because accept() blocks. The
// system asked to sleep, waited forever for a reply that could not come, and
// the console hung with a black screen — indistinguishable from a brick, and it
// needed a 15-second power-button hold to recover.
//
// So: a dedicated thread, blocking on the psc event, acknowledging every
// request unconditionally and immediately. It never touches a socket, never
// takes a lock, and has no path on which it can decline to answer.
void AckThread(void*) {
    while (!g_stop) {
        // Bounded wait so g_stop is honoured even with no transitions.
        Result rc = eventWait(&g_module.event, 1'000'000'000ULL);
        if (R_FAILED(rc)) continue;   // timeout: just loop

        PscPmState state;
        u32 flags = 0;
        while (R_SUCCEEDED(pscPmModuleGetRequest(&g_module, &state, &flags))) {
            switch (state) {
                case PscPmState_ReadySleep:
                case PscPmState_ReadySleepCritical:
                    g_asleep = true;
                    LOG_INFO("psc: system sleeping");
                    break;
                case PscPmState_Awake:
                case PscPmState_ReadyAwaken:
                case PscPmState_ReadyAwakenCritical:
                    if (g_asleep) LOG_INFO("psc: system awake");
                    g_asleep = false;
                    g_woke = true;
                    break;
                case PscPmState_ReadyShutdown:
                    LOG_INFO("psc: system shutting down");
                    break;
            }
            // Unconditional. Never add a branch that can skip this.
            pscPmModuleAcknowledge(&g_module, state);
        }
    }
}

bool Init(const AgentConfig& cfg) {
    // OFF unless explicitly enabled. Registering as a power-state module makes
    // the system WAIT for this process on every sleep, so a bug here does not
    // degrade the agent, it hangs the console. The accept()-error heuristic in
    // Server::Run recovers a few seconds slower and cannot do that, so it stays
    // the default.
    if (!cfg.enable_psc) {
        LOG_INFO("psc: disabled (enable_psc=false); using accept()-error recovery");
        return false;
    }
    if (R_FAILED(pscmInitialize())) {
        LOG_WARN("psc:m unavailable; falling back to accept()-error detection");
        return false;
    }
    if (R_FAILED(pscmGetPmModule(&g_module, kModuleId, nullptr, 0, true))) {
        LOG_WARN("could not register psc power module");
        pscmExit();
        return false;
    }
    g_stop = false;
    if (R_FAILED(threadCreate(&g_thread, AckThread, nullptr, nullptr, 0x2000, 49, 3)) ||
        R_FAILED(threadStart(&g_thread))) {
        // Critically: if the thread cannot start we must NOT stay registered,
        // or the system will wait on a module that can never answer.
        LOG_ERROR("psc: ack thread failed to start; unregistering to avoid a hang");
        pscPmModuleFinalize(&g_module);
        pscPmModuleClose(&g_module);
        pscmExit();
        return false;
    }
    g_registered = true;
    LOG_INFO("psc: registered, ack thread running");
    return true;
}

void Exit() {
    if (!g_registered) return;
    g_stop = true;
    threadWaitForExit(&g_thread);
    threadClose(&g_thread);
    pscPmModuleFinalize(&g_module);
    pscPmModuleClose(&g_module);
    pscmExit();
    g_registered = false;
}

bool Registered() { return g_registered; }
bool IsAsleep() { return g_asleep; }

// Consume the "we just woke" edge, so the accept loop can rebuild its listener
// immediately instead of waiting for errors to accumulate.
bool TakeWokeFlag() {
    if (!g_woke) return false;
    g_woke = false;
    return true;
}

}  // namespace power_state
}  // namespace agent
