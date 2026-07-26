// Power & system control: reboot, shutdown, reboot-to-payload, brightness,
// clocks, and agent self-info. Destructive actions are surfaced to the MCP
// client with clear names so it confirms with the user first.
#include "../protocol.hpp"

#include <switch.h>

#include <cstring>

#include "../log.hpp"

namespace agent {
namespace handlers {

namespace {

// Power transitions take effect immediately, so calling bpc from the handler
// kills the process before the server can write the reply frame — the client
// sees a dropped connection and cannot distinguish success from failure. We
// defer the transition onto a short-lived thread instead, giving the reply time
// to reach the wire.
enum class PowerAction { Reboot, Shutdown };

PowerAction g_pending_action = PowerAction::Reboot;

void PowerThread(void*) {
    svcSleepThread(500'000'000ULL);  // 500 ms: ample for a small reply frame
    if (g_pending_action == PowerAction::Shutdown) {
        bpcShutdownSystem();
    } else {
        bpcRebootSystem();
    }
}

// Schedule `action` to happen shortly. Returns false if the thread could not be
// started, in which case nothing has been done and the caller should report a
// plain failure rather than leaving the client guessing.
bool SchedulePowerAction(PowerAction action) {
    static Thread thread;
    static bool started = false;
    if (started) return true;  // one transition is enough

    g_pending_action = action;
    // Priority 49 matches the main thread; core 3 is the only one we may use.
    if (R_FAILED(threadCreate(&thread, PowerThread, nullptr, nullptr, 0x2000, 49, 3)))
        return false;
    if (R_FAILED(threadStart(&thread))) {
        threadClose(&thread);
        return false;
    }
    started = true;
    return true;
}

}  // namespace

bool Reboot(const Request& req, Reply& reply) {
    (void)req;
    LOG_WARN("reboot requested");
    if (!SchedulePowerAction(PowerAction::Reboot))
        return Fail(reply, "bpc_failed", "could not schedule reboot");
    reply.json.set("ok", true);
    reply.json.set("note", "rebooting in ~500ms");
    return true;
}

bool Shutdown(const Request& req, Reply& reply) {
    (void)req;
    LOG_WARN("shutdown requested");
    if (!SchedulePowerAction(PowerAction::Shutdown))
        return Fail(reply, "bpc_failed", "could not schedule shutdown");
    reply.json.set("ok", true);
    reply.json.set("note", "powering off in ~500ms");
    return true;
}

bool RebootToPayload(const Request& req, Reply& reply) {
    (void)req;
    // Atmosphère's bpc:ams reboots to the configured payload (hekate/fusee),
    // useful for recovery. Requires the ams bpc extension; without it this is
    // an ordinary reboot, which is why the reply says which one happened.
    LOG_WARN("reboot-to-payload requested");
    if (!SchedulePowerAction(PowerAction::Reboot))
        return Fail(reply, "bpc_failed", "could not schedule reboot");
    reply.json.set("ok", true);
    reply.json.set("note", "plain reboot (bpc:ams payload reboot not wired up yet)");
    return true;
}

bool SetBrightness(const Request& req, Reply& reply) {
    float level = (float)req["level"].as_double(0.5);  // 0.0 .. 1.0
    if (level < 0) level = 0;
    if (level > 1) level = 1;
    Result rc = lblSetCurrentBrightnessSetting(level);
    if (R_FAILED(rc)) return Fail(reply, "lbl_failed", "brightness set failed");
    // Ensure auto-brightness isn't fighting us.
    lblSwitchBacklightOn(0);
    reply.json.set("ok", true);
    reply.json.set("level", (double)level);
    return true;
}

bool SetClocks(const Request& req, Reply& reply) {
    if (!req.cfg.allow_overclock)
        return Fail(reply, "disabled", "clock control disabled in config");
    // Module IDs: cpu/gpu/emc. Values in Hz.
    struct {
        const char* key;
        PcvModule module;
    } modules[] = {
        {"cpu_hz", PcvModule_CpuBus},
        {"gpu_hz", PcvModule_GPU},
        {"emc_hz", PcvModule_EMC},
    };
    json::Value applied = json::Value::object();
    for (auto& m : modules) {
        if (!req.msg.has(m.key)) continue;
        u32 hz = (u32)req[m.key].as_int(0);
        if (hz == 0) continue;
        if (R_SUCCEEDED(pcvSetClockRate(m.module, hz))) applied.set(m.key, (int64_t)hz);
    }
    reply.json.set("ok", true);
    reply.json.set("applied", std::move(applied));
    return true;
}

// Set auto-sleep plans. Default disables auto-sleep entirely (both handheld and
// docked → Never), which is what keeps a headless console reachable. Persists
// across reboots (writes system settings).
bool SetSleep(const Request& req, Reply& reply) {
    SetSysSleepSettings s{};
    if (R_FAILED(setsysGetSleepSettings(&s)))
        return Fail(reply, "setsys_failed", "cannot read sleep settings");
    if (req["disable"].as_bool(true)) {
        s.handheld_sleep_plan = SetSysHandheldSleepPlan_Never;
        s.console_sleep_plan = SetSysConsoleSleepPlan_Never;
    } else {
        if (req.msg.has("handheld_plan"))
            s.handheld_sleep_plan = (s32)req["handheld_plan"].as_int();
        if (req.msg.has("console_plan"))
            s.console_sleep_plan = (s32)req["console_plan"].as_int();
    }
    if (R_FAILED(setsysSetSleepSettings(&s)))
        return Fail(reply, "setsys_failed", "cannot write sleep settings");
    LOG_INFO("sleep plans set: handheld=%d console=%d", s.handheld_sleep_plan,
             s.console_sleep_plan);
    reply.json.set("ok", true);
    reply.json.set("handheld_sleep_plan", (int64_t)s.handheld_sleep_plan);
    reply.json.set("console_sleep_plan", (int64_t)s.console_sleep_plan);
    return true;
}

bool AgentRestart(const Request& req, Reply& reply) {
    (void)req;
    // A sysmodule can't re-exec itself; the supported reload path is to write a
    // new exefs.nsp to SD (via fs.upload) and reboot. So "restart" == reboot.
    LOG_WARN("agent.restart requested (reboot to reload module)");
    // Deferred, same as Reboot: calling bpc inline kills us before the reply
    // frame is written, so the client can never tell success from a dropped
    // link.
    if (!SchedulePowerAction(PowerAction::Reboot))
        return Fail(reply, "bpc_failed", "could not schedule reboot");
    reply.json.set("ok", true);
    reply.json.set("note", "rebooting to reload the agent");
    return true;
}

bool AgentInfo(const Request& req, Reply& reply) {
    (void)req;
    reply.json.set("agent_version", "0.2.0");
    reply.json.set("protocol_version", 1);
    SetSysFirmwareVersion fw{};
    if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%u.%u.%u", fw.major, fw.minor, fw.micro);
        reply.json.set("firmware", buf);
    }
    reply.json.set("nand_write_allowed", req.cfg.allow_nand_write);
    reply.json.set("overclock_allowed", req.cfg.allow_overclock);
    return true;
}

}  // namespace handlers
}  // namespace agent
