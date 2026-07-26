// Brick guards: preflight checks, an audit journal, and a boot watchdog.
//
// The tier system in server.cpp decides *who* may do a thing. This decides
// *whether now is a safe moment* to do it, and leaves a trail when it happens.
//
// Ordering of what can actually destroy this console, worst first: BOOT0/BOOT1
// (bootloader), PRODINFO/CAL0 (device certificates, unrecoverable without a
// backup), BIS System (boot loop), fuses, PMIC i2c writes (physical damage),
// overclock (thermal). Note the operator here has NO NAND backups, so the
// honest guard for the top of that list is "refuse", not "warn" — which is why
// allow_nand_write stays off and the raw BIS write path is not implemented at
// all rather than implemented-and-gated.
#include "../protocol.hpp"

#include <switch.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>

#include "../log.hpp"

namespace agent {
namespace handlers {

namespace {

constexpr const char* kJournalPath = "sdmc:/switch-agentd/journal.log";
constexpr const char* kBootCountPath = "sdmc:/switch-agentd/bootcount";
constexpr int kJournalMaxBytes = 512 * 1024;

// Writing NAND or rebooting to a payload on a nearly flat battery is a classic
// way to end up with a half-written partition and a console that will not boot.
constexpr u32 kMinBatteryPercentForRiskyOps = 30;

void EnsureDir() {
    mkdir("sdmc:/switch-agentd", 0777);
}

}  // namespace

// Append-only audit trail of anything that changed the console.
//
// Deliberately on the SD card and not in memory: the interesting case is
// working out what happened *before* the console stopped booting, which is
// exactly when nothing in RAM survives.
void JournalAppend(const char* op, const std::string& detail) {
    EnsureDir();
    FILE* f = std::fopen(kJournalPath, "a");
    if (!f) return;
    // Uptime rather than wall clock: the RTC on a CFW console is frequently
    // wrong, and a monotonic ordering is what matters for reconstructing a
    // sequence of events.
    u64 ticks = armGetSystemTick();
    u64 secs = ticks / armGetSystemTickFreq();
    std::fprintf(f, "[%6llu] %-20s %s\n", (unsigned long long)secs, op,
                 detail.c_str());
    std::fclose(f);

    struct stat st;
    if (stat(kJournalPath, &st) == 0 && st.st_size > kJournalMaxBytes) {
        // Single rotation. An unbounded journal on the same card that holds the
        // games is its own hazard.
        std::rename(kJournalPath, "sdmc:/switch-agentd/journal.log.1");
    }
}

// Is now a safe moment for something irreversible?
bool PreflightCheck(const Request& req, Reply& reply) {
    (void)req;
    bool safe = true;
    json::Value problems = json::Value::array();

    u32 charge = 0;
    Result rc = psmGetBatteryChargePercentage(&charge);
    if (R_SUCCEEDED(rc)) {
        reply.json.set("battery_percent", (int64_t)charge);
        if (charge < kMinBatteryPercentForRiskyOps) {
            safe = false;
            problems.push(json::Value(
                "battery below " + std::to_string(kMinBatteryPercentForRiskyOps) +
                "%: a power loss part-way through a NAND write or a payload "
                "reboot can leave the console unbootable"));
        }
    }
    PsmChargerType ct = PsmChargerType_Unconnected;
    if (R_SUCCEEDED(psmGetChargerType(&ct))) {
        reply.json.set("on_charger", ct != PsmChargerType_Unconnected);
        if (ct == PsmChargerType_Unconnected && charge < 50)
            problems.push(json::Value(
                "not on charger; plug in before anything irreversible"));
    }

    // Free space on the SD card: a backup or dump that runs out of room
    // half-way is worse than one that never started.
    s64 free_bytes = 0;
    if (R_SUCCEEDED(fsFsGetFreeSpace(fsdevGetDeviceFileSystem("sdmc"), "/",
                                     &free_bytes))) {
        reply.json.set("sd_free_bytes", (int64_t)free_bytes);
        if (free_bytes < (1LL << 30)) {
            problems.push(json::Value(
                "under 1 GiB free on the SD card: not enough room for a "
                "meaningful backup"));
        }
    }

    reply.json.set("emummc", req.cfg.is_emummc);
    if (!req.cfg.is_emummc) {
        problems.push(json::Value(
            "running on sysMMC: a mistake here is NOT recoverable by restoring "
            "an SD image, unlike emuMMC"));
    }
    reply.json.set("nand_write_allowed", req.cfg.allow_nand_write);
    reply.json.set("safe_for_irreversible_ops", safe);
    reply.json.set("warnings", std::move(problems));
    return true;
}

// Read the audit journal back.
bool JournalRead(const Request& req, Reply& reply) {
    int max_bytes = (int)req["max_bytes"].as_int(16384);
    if (max_bytes < 256 || max_bytes > 48000) max_bytes = 16384;

    FILE* f = std::fopen(kJournalPath, "rb");
    if (!f) {
        reply.json.set("content", "");
        reply.json.set("note", "no journal yet (nothing mutating has been logged)");
        return true;
    }
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    long start = size > max_bytes ? size - max_bytes : 0;
    std::fseek(f, start, SEEK_SET);
    std::string content;
    content.resize((size_t)(size - start));
    size_t got = std::fread(&content[0], 1, content.size(), f);
    content.resize(got);
    std::fclose(f);

    reply.json.set("total_bytes", (int64_t)size);
    reply.json.set("truncated", start > 0);
    reply.json.set("content", content);
    return true;
}

// --- boot watchdog -----------------------------------------------------------
//
// Counts boots that never saw a successful client connection. The point is to
// notice "the last change I made stops this console being reachable" without
// needing someone to physically pull the SD card to find out — which is
// precisely the situation a 16 MiB heap change put this console in.

static int g_boot_count = 0;

void WatchdogOnBoot() {
    EnsureDir();
    FILE* f = std::fopen(kBootCountPath, "rb");
    if (f) {
        char buf[16] = {0};
        std::fread(buf, 1, sizeof(buf) - 1, f);
        std::fclose(f);
        g_boot_count = std::atoi(buf);
    }
    g_boot_count++;
    if (FILE* w = std::fopen(kBootCountPath, "wb")) {
        std::fprintf(w, "%d", g_boot_count);
        std::fclose(w);
    }
    if (g_boot_count >= 3) {
        LOG_WARN("watchdog: %d boots with no successful client connection. "
                 "If this console has become unreachable, the most recent "
                 "config or agent change is the first thing to undo.",
                 g_boot_count);
        JournalAppend("watchdog", "boot #" + std::to_string(g_boot_count) +
                                      " with no prior successful connection");
    }
}

// Called once a client authenticates: the agent is demonstrably reachable, so
// the streak resets.
void WatchdogOnConnect() {
    if (g_boot_count == 0) return;
    g_boot_count = 0;
    if (FILE* w = std::fopen(kBootCountPath, "wb")) {
        std::fprintf(w, "0");
        std::fclose(w);
    }
}

bool WatchdogStatus(const Request& req, Reply& reply) {
    (void)req;
    reply.json.set("boots_without_connection", (int64_t)g_boot_count);
    reply.json.set("healthy", g_boot_count == 0);
    return true;
}

}  // namespace handlers
}  // namespace agent
