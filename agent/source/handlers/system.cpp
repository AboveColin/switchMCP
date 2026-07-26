// Additional system tools: brightness/volume/wireless/time/console-info/fs-copy.
#include "../protocol.hpp"

#include <switch.h>

#include <dirent.h>
#include <sys/stat.h>

#include <cstdio>
#include <string>

#include "../log.hpp"

namespace agent {
namespace handlers {

bool GetBrightness(const Request& req, Reply& reply) {
    (void)req;
    float level = 0;
    if (R_FAILED(lblGetCurrentBrightnessSetting(&level)))
        return Fail(reply, "lbl_failed", "cannot read brightness");
    reply.json.set("level", (double)level);
    return true;
}

bool SetAutoBrightness(const Request& req, Reply& reply) {
    bool enable = req["enable"].as_bool(true);
    Result rc = enable ? lblEnableAutoBrightnessControl()
                       : lblDisableAutoBrightnessControl();
    if (R_FAILED(rc)) return Fail(reply, "lbl_failed", "auto-brightness toggle failed");
    reply.json.set("ok", true);
    reply.json.set("auto_brightness", enable);
    return true;
}

bool GetVolume(const Request& req, Reply& reply) {
    (void)req;
    float vol = 0;
    if (R_FAILED(audctlGetSystemOutputMasterVolume(&vol)))
        return Fail(reply, "audctl_failed", "cannot read volume");
    reply.json.set("volume", (double)vol);
    return true;
}

bool SetVolume(const Request& req, Reply& reply) {
    float vol = (float)req["volume"].as_double(0.5);
    if (vol < 0) vol = 0;
    if (vol > 1) vol = 1;
    if (R_FAILED(audctlSetSystemOutputMasterVolume(vol)))
        return Fail(reply, "audctl_failed", "cannot set volume");
    reply.json.set("ok", true);
    reply.json.set("volume", (double)vol);
    return true;
}

bool SetWireless(const Request& req, Reply& reply) {
    bool enable = req["enable"].as_bool(true);
    if (R_FAILED(nifmSetWirelessCommunicationEnabled(enable)))
        return Fail(reply, "nifm_failed", "cannot toggle wireless");
    reply.json.set("ok", true);
    reply.json.set("wireless_enabled", enable);
    return true;
}

bool GetTime(const Request& req, Reply& reply) {
    (void)req;
    u64 ts = 0;
    if (R_FAILED(timeGetCurrentTime(TimeType_Default, &ts)))
        return Fail(reply, "time_failed", "cannot read time");
    reply.json.set("unix_time", (int64_t)ts);
    return true;
}

bool SetTime(const Request& req, Reply& reply) {
    int64_t ts = req["unix_time"].as_int(0);
    if (ts <= 0) return Fail(reply, "bad_arg", "missing unix_time");
    if (R_FAILED(timeSetCurrentTime(TimeType_UserSystemClock, (u64)ts)))
        return Fail(reply, "time_failed", "cannot set time");
    reply.json.set("ok", true);
    reply.json.set("unix_time", ts);
    return true;
}

bool ConsoleInfo(const Request& req, Reply& reply) {
    (void)req;
    SetSysSerialNumber serial{};
    if (R_SUCCEEDED(setsysGetSerialNumber(&serial)))
        reply.json.set("serial", serial.number);
    SetSysDeviceNickName nick{};
    if (R_SUCCEEDED(setsysGetDeviceNickname(&nick)))
        reply.json.set("nickname", nick.nickname);
    ColorSetId color;
    if (R_SUCCEEDED(setsysGetColorSetId(&color)))
        reply.json.set("theme", color == ColorSetId_Dark ? "dark" : "light");
    // Region code comes from the `set` service (initialized in main).
    SetRegion region;
    if (R_SUCCEEDED(setGetRegionCode(&region))) {
        static const char* names[] = {"JPN", "USA", "EUR", "AUS", "HTK", "CHN"};
        int r = (int)region;
        reply.json.set("region", (r >= 0 && r < 6) ? names[r] : "unknown");
    }
    return true;
}

bool GetClocks(const Request& req, Reply& reply) {
    (void)req;
    // pcvGetClockRate is deprecated on modern firmware; clock reads go through
    // clkrst sessions instead.
    struct {
        const char* key;
        PcvModuleId module;
    } mods[] = {{"cpu_hz", PcvModuleId_CpuBus}, {"gpu_hz", PcvModuleId_GPU},
                {"emc_hz", PcvModuleId_EMC}};
    for (auto& m : mods) {
        ClkrstSession sess;
        if (R_FAILED(clkrstOpenSession(&sess, m.module, 3))) continue;
        u32 hz = 0;
        if (R_SUCCEEDED(clkrstGetClockRate(&sess, &hz))) reply.json.set(m.key, (int64_t)hz);
        clkrstCloseSession(&sess);
    }
    return true;
}

bool Controllers(const Request& req, Reply& reply) {
    (void)req;
    // hid is initialized + npad-configured once in main(). Reading npad state
    // from a background sysmodule can be limited (no applet foreground
    // resource), but connection + power info is generally available.
    static const HidNpadIdType ids[] = {
        HidNpadIdType_No1, HidNpadIdType_No2, HidNpadIdType_No3, HidNpadIdType_No4,
        HidNpadIdType_No5, HidNpadIdType_No6, HidNpadIdType_No7, HidNpadIdType_No8,
        HidNpadIdType_Handheld};

    json::Value list = json::Value::array();
    for (HidNpadIdType id : ids) {
        u32 style = hidGetNpadStyleSet(id);
        if (style == 0) continue;  // not connected
        HidPowerInfo pi{};
        hidGetNpadPowerInfoSingle(id, &pi);
        json::Value c = json::Value::object();
        c.set("id", id == HidNpadIdType_Handheld ? (int64_t)0x20 : (int64_t)id);
        c.set("handheld", id == HidNpadIdType_Handheld);
        c.set("style_set", (int64_t)style);
        c.set("battery", (int64_t)pi.battery_level);
        c.set("charging", (bool)pi.is_charging);
        c.set("powered", (bool)pi.is_powered);
        list.push(std::move(c));
    }
    reply.json.set("controllers", std::move(list));
    return true;
}

bool FsCopy(const Request& req, Reply& reply) {
    // Server maps '/' to sdmc:. Copy within the SD card, on-device (fast — no
    // round-trip through the client).
    // Go through the shared normalizer rather than concatenating "sdmc:"
    // ourselves: one path policy, audited in one place.
    std::string device = req["device"].as_string("sd");
    if (device != "sd" && device != "sdmc")
        return Fail(reply, "read_only_device",
                    "fs.copy writes, so it only works on the SD card "
                    "(device=\"sd\")");
    std::string src, dst;
    if (!ResolveSdPath(req["from"].as_string(), src) ||
        !ResolveSdPath(req["to"].as_string(), dst))
        return Fail(reply, "bad_path", "invalid or unsafe path");
    FILE* in = std::fopen(src.c_str(), "rb");
    if (!in) return Fail(reply, "not_found", "source not found");
    FILE* out = std::fopen(dst.c_str(), "wb");
    if (!out) {
        std::fclose(in);
        return Fail(reply, "io_error", "cannot open destination");
    }
    // Heap, not stack: main_thread_stack_size is 32 KiB, so a 64 KiB local
    // array overflows the stack in this function's prologue and kills the
    // sysmodule before a single line of the body runs.
    std::vector<char> buf(64 * 1024);
    size_t n, total = 0;
    while ((n = std::fread(buf.data(), 1, buf.size(), in)) > 0) {
        total += std::fwrite(buf.data(), 1, n, out);
    }
    std::fclose(in);
    std::fclose(out);
    reply.json.set("ok", true);
    reply.json.set("bytes", (int64_t)total);
    return true;
}


// Read the current auto-sleep configuration, including the raw flag bitmask.
// Read-only counterpart to set_sleep: needed to tell "the user disabled sleep"
// from "the console still thinks it should sleep".
bool GetSleep(const Request& req, Reply& reply) {
    (void)req;
    SetSysSleepSettings s{};
    if (R_FAILED(setsysGetSleepSettings(&s)))
        return Fail(reply, "setsys_failed", "cannot read sleep settings");
    static const char* kHandheld[] = {"1min","3min","5min","10min","30min","never"};
    static const char* kConsole[]  = {"1h","2h","3h","6h","12h","never"};
    reply.json.set("handheld_sleep_plan", (int64_t)s.handheld_sleep_plan);
    reply.json.set("console_sleep_plan", (int64_t)s.console_sleep_plan);
    if (s.handheld_sleep_plan >= 0 && s.handheld_sleep_plan <= 5)
        reply.json.set("handheld", kHandheld[s.handheld_sleep_plan]);
    if (s.console_sleep_plan >= 0 && s.console_sleep_plan <= 5)
        reply.json.set("console", kConsole[s.console_sleep_plan]);
    reply.json.set("flags", (int64_t)s.flags);
    reply.json.set("sleeps_while_playing_media", (bool)(s.flags & 1));
    reply.json.set("wakes_at_power_state_change", (bool)(s.flags & 2));
    return true;
}

// Read an arbitrary system settings item by (name, key).
//
// The Settings service exposes hundreds of tunables that have no dedicated
// libnx wrapper - including the ones behind UI toggles whose mapping is not
// documented. Being able to read any of them turns "which setting is this?"
// from guesswork into a lookup. Read-only by design: writing arbitrary system
// settings is a good way to make a console misbehave in confusing ways.
bool SettingsGet(const Request& req, Reply& reply) {
    std::string name = req["name"].as_string();
    std::string key = req["key"].as_string();
    if (name.empty() || key.empty())
        return Fail(reply, "bad_arg", "need both 'name' and 'key'");

    u64 size = 0;
    Result rc = setsysGetSettingsItemValueSize(name.c_str(), key.c_str(), &size);
    if (R_FAILED(rc))
        return Fail(reply, "not_found",
                    "no such settings item (rc=" + std::to_string(rc) + ")");
    if (size == 0 || size > 65536)
        return Fail(reply, "bad_size",
                    "settings item exists but has size " + std::to_string(size));

    std::vector<uint8_t> buf(size);
    u64 got = 0;
    rc = setsysGetSettingsItemValue(name.c_str(), key.c_str(), buf.data(), size, &got);
    if (R_FAILED(rc)) return Fail(reply, "read_failed", "cannot read settings item");
    buf.resize(got);

    reply.json.set("name", name);
    reply.json.set("key", key);
    reply.json.set("size", (int64_t)got);
    // Small items are almost always a bool or an integer; surface both a
    // decoded view and the raw bytes so the caller can interpret either way.
    if (got >= 1 && got <= 8) {
        uint64_t v = 0;
        for (size_t i = 0; i < got; i++) v |= (uint64_t)buf[i] << (8 * i);
        reply.json.set("value", (int64_t)v);
        if (got == 1) reply.json.set("as_bool", buf[0] != 0);
    }
    std::string hex;
    static const char* hd = "0123456789abcdef";
    for (uint8_t b : buf) { hex += hd[b >> 4]; hex += hd[b & 0xF]; }
    reply.json.set("hex", hex);
    // Printable payloads are usually strings.
    bool printable = !buf.empty();
    for (uint8_t b : buf) if (b && (b < 0x20 || b > 0x7E)) { printable = false; break; }
    if (printable) reply.json.set("as_string", std::string((char*)buf.data(), got));
    return true;
}


// Network configuration as the console sees it.
//
// Directly useful here: this console is reached over Wi-Fi, its address comes
// from DHCP, and "did the IP change?" has been a recurring question. Reporting
// the address, mask, gateway and DNS from the device removes the guessing.
bool NetInfo(const Request& req, Reply& reply) {
    (void)req;
    u32 ip = 0;
    if (R_SUCCEEDED(nifmGetCurrentIpAddress(&ip))) {
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip & 0xFF, (ip >> 8) & 0xFF,
                      (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
        reply.json.set("ip", buf);
    }

    u32 cur = 0, mask = 0, gw = 0, dns1 = 0, dns2 = 0;
    if (R_SUCCEEDED(nifmGetCurrentIpConfigInfo(&cur, &mask, &gw, &dns1, &dns2))) {
        auto fmt = [&](const char* key, u32 v) {
            char b[24];
            std::snprintf(b, sizeof(b), "%u.%u.%u.%u", v & 0xFF, (v >> 8) & 0xFF,
                          (v >> 16) & 0xFF, (v >> 24) & 0xFF);
            reply.json.set(key, b);
        };
        fmt("address", cur);
        fmt("subnet_mask", mask);
        fmt("gateway", gw);
        fmt("dns_primary", dns1);
        fmt("dns_secondary", dns2);
    }

    bool enabled = false;
    if (R_SUCCEEDED(nifmIsWirelessCommunicationEnabled(&enabled)))
        reply.json.set("wireless_enabled", enabled);
    reply.json.set("internet_request_accepted",
                   nifmIsAnyInternetRequestAccepted(nifmGetClientId()));
    return true;
}

// Atmosphère fatal reports, which land in a different directory from crash
// reports and are what you get when the whole system goes down rather than one
// process. Complements crash_reports rather than replacing it.
bool FatalReports(const Request& req, Reply& reply) {
    const char* dir = "sdmc:/atmosphere/fatal_reports";
    std::string name = req["name"].as_string();

    if (!name.empty()) {
        if (name.find('/') != std::string::npos || name.find("..") != std::string::npos)
            return Fail(reply, "bad_path", "name must be a bare filename");
        std::string full = std::string(dir) + "/" + name;
        FILE* f = std::fopen(full.c_str(), "rb");
        if (!f) return Fail(reply, "not_found", "no such fatal report");
        std::string content;
        char buf[4096];
        size_t n;
        // Capped: a fatal report with a full register dump can be large, and the
        // frame limit is 64 KiB.
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0 && content.size() < 48000)
            content.append(buf, n);
        std::fclose(f);
        reply.json.set("name", name);
        reply.json.set("content", content);
        return true;
    }

    DIR* d = opendir(dir);
    if (!d) {
        reply.json.set("reports", json::Value::array());
        reply.json.set("note", "no fatal_reports directory (nothing has gone fatal)");
        return true;
    }
    json::Value arr = json::Value::array();
    struct dirent* de;
    int count = 0;
    while ((de = readdir(d)) != nullptr && count < 200) {
        std::string n2 = de->d_name;
        if (n2 == "." || n2 == ".." || n2 == "dumps") continue;
        struct stat st;
        std::string full = std::string(dir) + "/" + n2;
        json::Value e = json::Value::object();
        e.set("name", n2);
        if (stat(full.c_str(), &st) == 0) e.set("size", (int64_t)st.st_size);
        arr.push(std::move(e));
        count++;
    }
    closedir(d);
    reply.json.set("count", (int64_t)count);
    reply.json.set("reports", std::move(arr));
    return true;
}

}  // namespace handlers
}  // namespace agent
