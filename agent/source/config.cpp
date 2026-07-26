// config.ini loader for switch-agentd. Simple `key = value` INI, '#'/';'
// comments. Generates a random token on first boot if none is set.
#include "protocol.hpp"

#include <switch.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>

#include "log.hpp"

namespace agent {

namespace {

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// 32-char hex token from the hardware RNG (csrng via libnx randomGet).
std::string RandomToken() {
    uint8_t buf[16];
    randomGet(buf, sizeof(buf));
    static const char* hex = "0123456789abcdef";
    std::string out;
    for (uint8_t b : buf) {
        out += hex[b >> 4];
        out += hex[b & 0xF];
    }
    return out;
}

void EnsureDir(const char* path) {
    // mkdir -p for a single-level config dir; ignore EEXIST.
    mkdir(path, 0777);
}

Tier ParseTier(const std::string& v) {
    if (v == "control") return Tier::Control;
    if (v == "invasive") return Tier::Invasive;
    return Tier::Observe;  // unknown values fail safe
}

// Detect whether Atmosphère booted an emuMMC rather than the internal eMMC.
//
// This matters more than any config flag: on emuMMC the entire "system" is a
// file or partition on the SD card, so even a destroyed boot partition is fixed
// by restoring a backup image. On sysMMC the same write can permanently brick
// the console. We therefore let emuMMC relax the hardest guards.
//
// Atmosphère writes `emummc/emummc.ini` and, when active, exposes the setting
// through the ams:bpc/exosphere config. Reading the ini is the portable check
// that does not depend on which extension IPC this libnx build has: the
// [emummc] section carries `enabled=1` when an emuMMC is in use.
bool DetectEmummc() {
    FILE* f = std::fopen("sdmc:/emuMMC/emummc.ini", "r");
    if (!f) f = std::fopen("sdmc:/emummc/emummc.ini", "r");
    if (!f) return false;

    bool enabled = false;
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        std::string s = Trim(line);
        if (s.empty() || s[0] == '#' || s[0] == ';' || s[0] == '[') continue;
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        if (Trim(s.substr(0, eq)) == "enabled") {
            std::string v = Trim(s.substr(eq + 1));
            enabled = (v != "0" && !v.empty());
            break;
        }
    }
    std::fclose(f);
    return enabled;
}

}  // namespace

AgentConfig LoadConfig(const char* path) {
    AgentConfig cfg;
    std::string log_path = "sdmc:/config/switch-agentd/agent.log";

    FILE* f = std::fopen(path, "r");
    if (f) {
        char line[512];
        while (std::fgets(line, sizeof(line), f)) {
            std::string s = Trim(line);
            if (s.empty() || s[0] == '#' || s[0] == ';') continue;
            size_t eq = s.find('=');
            if (eq == std::string::npos) continue;
            std::string key = Trim(s.substr(0, eq));
            std::string val = Trim(s.substr(eq + 1));
            if (key == "port") {
                cfg.port = (uint16_t)std::strtoul(val.c_str(), nullptr, 10);
            } else if (key == "token") {
                cfg.token = val;
            } else if (key == "log_level") {
                if (val == "error") cfg.log_level = 0;
                else if (val == "warn") cfg.log_level = 1;
                else if (val == "debug") cfg.log_level = 3;
                else cfg.log_level = 2;  // info
            } else if (key == "allow_nand_write") {
                cfg.allow_nand_write = (val == "true" || val == "1");
            } else if (key == "allow_overclock") {
                cfg.allow_overclock = (val == "true" || val == "1");
            } else if (key == "allow_hardware") {
                cfg.allow_hardware = (val == "true" || val == "1");
            } else if (key == "tier") {
                cfg.tier = ParseTier(val);
            } else if (key == "clear_lockscreen_on_boot") {
                cfg.clear_lockscreen_on_boot = (val == "true" || val == "1");
            } else if (key == "enable_usb") {
                cfg.enable_usb = (val == "true" || val == "1");
            } else if (key == "enable_psc") {
                cfg.enable_psc = (val == "true" || val == "1");
            } else if (key == "require_hmac_auth") {
                cfg.require_hmac_auth = (val == "true" || val == "1");
            } else if (key == "keep_awake_minutes") {
                cfg.keep_awake_minutes = (int)std::strtol(val.c_str(), nullptr, 10);
                if (cfg.keep_awake_minutes < 0) cfg.keep_awake_minutes = 0;
            }
        }
        std::fclose(f);
    }

    // First-boot: no config or empty token → generate one and persist it so the
    // user can read it off the SD card. Never run with an empty token.
    if (cfg.token.empty()) {
        cfg.token = RandomToken();
        EnsureDir("sdmc:/config");
        EnsureDir("sdmc:/config/switch-agentd");
        if (FILE* w = std::fopen(path, "w")) {
            std::fprintf(w,
                         "# switch-agentd config (auto-generated)\n"
                         "port = %u\n"
                         "token = %s\n"
                         "log_level = info\n"
                         "\n"
                         "# Capability tier: observe | control | invasive\n"
                         "#   observe  - read-only; cannot change the console\n"
                         "#   control  - input, launch apps, SD-card files\n"
                         "#   invasive - NAND/BIS writes, raw i2c, memory patching\n"
                         "# Starts read-only on purpose. Raise it deliberately.\n"
                         "tier = observe\n"
                         "\n"
                         "# Clear the boot lockscreen automatically (needs tier >= control).\n"
                         "# The lockscreen sleeps even with auto-sleep set to Never, and\n"
                         "# sleeping drops the network, so a headless console left at the\n"
                         "# lockscreen becomes unreachable until someone power-cycles it.\n"
                         "clear_lockscreen_on_boot = true\n"
                         "\n"
                         "# Minutes between keep-awake nudges, 0 to disable. Stops the\n"
                         "# console re-locking and then sleeping while idle, which drops\n"
                         "# the network. Skipped while a game is running.\n"
                         "keep_awake_minutes = 10\n"
                         "\n"
                         "# Refuse the legacy cleartext-token handshake. The token then\n"
                         "# never crosses the network; clients prove knowledge of it by\n"
                         "# answering a server-issued nonce. Turn on once every client\n"
                         "# supports it (switch-mcp does).\n"
                         "require_hmac_auth = false\n"
                         "\n"
                         "# Extra opt-ins, each also requiring tier = invasive.\n"
                         "allow_nand_write = false\n"
                         "allow_overclock = false\n"
                         "allow_hardware = false\n",
                         cfg.port, cfg.token.c_str());
            std::fclose(w);
        }
    }

    log::Init(log_path, (log::Level)cfg.log_level);

    cfg.is_emummc = DetectEmummc();

    static const char* kTierNames[] = {"observe", "control", "invasive"};
    LOG_INFO("config loaded: port=%u token=%zu chars tier=%s emummc=%s "
             "nand_write=%d hardware=%d",
             cfg.port, cfg.token.size(), kTierNames[(int)cfg.tier],
             cfg.is_emummc ? "yes" : "no", cfg.allow_nand_write, cfg.allow_hardware);
    if (cfg.tier == Tier::Invasive && !cfg.is_emummc)
        LOG_WARN("invasive tier on sysMMC: destructive writes can brick this console");
    return cfg;
}

}  // namespace agent
