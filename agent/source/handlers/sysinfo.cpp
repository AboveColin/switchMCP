// Diagnostics: sysinfo, process list, crash reports, log tail.
// Each metric is gathered defensively — a service that fails just omits its
// field rather than failing the whole call.
#include "../protocol.hpp"

#include <switch.h>

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "../log.hpp"

namespace agent {
namespace handlers {

bool SysInfo(const Request& req, Reply& reply) {
    (void)req;
    json::Value info = json::Value::object();

    // Firmware
    SetSysFirmwareVersion fw{};
    if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw))) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "%u.%u.%u", fw.major, fw.minor, fw.micro);
        info.set("firmware", buf);
        info.set("firmware_display", fw.display_version);
    }

    // Uptime (system tick runs at a fixed 19.2 MHz; convert to seconds)
    info.set("uptime_s", (int64_t)(armTicksToNs(armGetSystemTick()) / 1'000'000'000ULL));

    // Battery
    u32 charge = 0;
    if (R_SUCCEEDED(psmGetBatteryChargePercentage(&charge)))
        info.set("battery_pct", (int64_t)charge);
    PsmChargerType charger = PsmChargerType_Unconnected;
    if (R_SUCCEEDED(psmGetChargerType(&charger))) {
        info.set("charging", charger != PsmChargerType_Unconnected);
        info.set("charger_type", (int64_t)charger);
    }

    // Temperatures. tsGetTemperatureMilliC is [1.0.0-13.2.1] only; on newer
    // firmware use the per-location session API. Note libnx labels
    // Internal=PCB, External=SoC.
    if (hosversionBefore(14, 0, 0)) {
        s32 milli = 0;
        if (R_SUCCEEDED(tsGetTemperatureMilliC(TsLocation_External, &milli)))
            info.set("temp_soc_c", milli / 1000.0);
        if (R_SUCCEEDED(tsGetTemperatureMilliC(TsLocation_Internal, &milli)))
            info.set("temp_pcb_c", milli / 1000.0);
    } else {
        TsSession sess;
        float t = 0;
        if (R_SUCCEEDED(tsOpenSession(&sess, TsDeviceCode_LocationExternal))) {
            if (R_SUCCEEDED(tsSessionGetTemperature(&sess, &t)))
                info.set("temp_soc_c", (double)t);
            tsSessionClose(&sess);
        }
        if (R_SUCCEEDED(tsOpenSession(&sess, TsDeviceCode_LocationInternal))) {
            if (R_SUCCEEDED(tsSessionGetTemperature(&sess, &t)))
                info.set("temp_pcb_c", (double)t);
            tsSessionClose(&sess);
        }
    }

    // SD storage free/total
    FsFileSystem* sd = fsdevGetDeviceFileSystem("sdmc");
    if (sd) {
        s64 freeb = 0, totalb = 0;
        json::Value storage = json::Value::object();
        if (R_SUCCEEDED(fsFsGetFreeSpace(sd, "/", &freeb)))
            storage.set("sd_free", (int64_t)freeb);
        if (R_SUCCEEDED(fsFsGetTotalSpace(sd, "/", &totalb)))
            storage.set("sd_total", (int64_t)totalb);
        info.set("storage", std::move(storage));
    }

    // Network: connection status + current IP
    json::Value net = json::Value::object();
    NifmInternetConnectionType type;
    u32 strength = 0;
    NifmInternetConnectionStatus status;
    if (R_SUCCEEDED(nifmGetInternetConnectionStatus(&type, &strength, &status))) {
        net.set("connected", status == NifmInternetConnectionStatus_Connected);
        net.set("signal", (int64_t)strength);
        net.set("connection_type", type == NifmInternetConnectionType_WiFi ? "wifi"
                                                                            : "ethernet");
    }
    u32 ip = 0;
    if (R_SUCCEEDED(nifmGetCurrentIpAddress(&ip))) {
        char ipbuf[16];
        std::snprintf(ipbuf, sizeof(ipbuf), "%u.%u.%u.%u", ip & 0xFF, (ip >> 8) & 0xFF,
                      (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
        net.set("ip", ipbuf);
    }
    info.set("network", std::move(net));

    // Agent's own memory footprint (system-wide RAM needs debug perms we avoid).
    u64 used = 0, total = 0;
    svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    json::Value mem = json::Value::object();
    mem.set("agent_used", (int64_t)used);
    mem.set("agent_total", (int64_t)total);
    info.set("memory", std::move(mem));

    reply.json = std::move(info);
    return true;
}

bool ProcessList(const Request& req, Reply& reply) {
    (void)req;
    u64 pids[300];
    s32 count = 0;
    Result rc = svcGetProcessList(&count, pids, 300);
    if (R_FAILED(rc)) return Fail(reply, "svc_failed", "svcGetProcessList failed");

    json::Value list = json::Value::array();
    for (s32 i = 0; i < count; i++) {
        json::Value p = json::Value::object();
        p.set("pid", (int64_t)pids[i]);
        u64 tid = 0;
        if (R_SUCCEEDED(pminfoGetProgramId(&tid, pids[i]))) {
            char tidbuf[20];
            std::snprintf(tidbuf, sizeof(tidbuf), "%016lx", (unsigned long)tid);
            p.set("tid", tidbuf);
        }
        list.push(std::move(p));
    }
    reply.json.set("processes", std::move(list));
    return true;
}

bool CrashReports(const Request& req, Reply& reply) {
    const char* dir_path = "sdmc:/atmosphere/crash_reports";

    // If a specific report is requested, return its contents (text).
    if (req.msg.has("name")) {
        std::string path = std::string(dir_path) + "/" + req["name"].as_string();
        // Guard against traversal.
        if (req["name"].as_string().find("..") != std::string::npos)
            return Fail(reply, "bad_path", "invalid name");
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return Fail(reply, "not_found", "no such crash report");
        std::string content;
        char buf[8192];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
            content.append(buf, n);
        std::fclose(f);
        reply.json.set("name", req["name"].as_string());
        reply.json.set("content", std::move(content));
        return true;
    }

    // Otherwise list available reports newest-first-ish (client can sort).
    json::Value list = json::Value::array();
    DIR* dir = opendir(dir_path);
    if (dir) {
        struct dirent* de;
        while ((de = readdir(dir)) != nullptr) {
            std::string name = de->d_name;
            if (name == "." || name == "..") continue;
            json::Value e = json::Value::object();
            e.set("name", name);
            struct stat st;
            if (stat((std::string(dir_path) + "/" + name).c_str(), &st) == 0) {
                e.set("size", (int64_t)st.st_size);
                e.set("mtime", (int64_t)st.st_mtime);
            }
            list.push(std::move(e));
        }
        closedir(dir);
    }
    reply.json.set("reports", std::move(list));
    return true;
}

bool ReadLog(const Request& req, Reply& reply) {
    std::string path = req["path"].as_string("sdmc:/config/switch-agentd/agent.log");
    // Only allow sdmc: paths; block traversal.
    if (path.rfind("sdmc:/", 0) != 0 || path.find("..") != std::string::npos)
        return Fail(reply, "bad_path", "only sdmc: paths without .. allowed");
    int64_t max_bytes = req["max_bytes"].as_int(16 * 1024);
    if (max_bytes <= 0 || max_bytes > 256 * 1024) max_bytes = 16 * 1024;

    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return Fail(reply, "not_found", "cannot open log");
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    long start = size > max_bytes ? size - max_bytes : 0;
    std::fseek(f, start, SEEK_SET);
    std::string content;
    content.resize(size - start);
    size_t got = std::fread(&content[0], 1, content.size(), f);
    content.resize(got);
    std::fclose(f);
    reply.json.set("content", std::move(content));
    reply.json.set("truncated", start > 0);
    return true;
}

}  // namespace handlers
}  // namespace agent
