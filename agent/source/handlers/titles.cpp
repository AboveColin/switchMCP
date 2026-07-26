// Title management: list installed titles, fetch icons, report/launch/terminate.
#include "../protocol.hpp"

#include <switch.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "../log.hpp"

namespace agent {
namespace handlers {

namespace {
void TidToHex(u64 tid, char out[17]) {
    std::snprintf(out, 17, "%016lx", (unsigned long)tid);
}

// Fetch NACP + icon for a title. control_data is large (~0x20000) so it's
// heap-allocated by the caller and reused.
Result LoadControl(u64 tid, NsApplicationControlData* data, u64* out_size) {
    return nsGetApplicationControlData(NsApplicationControlSource_Storage, tid, data,
                                       sizeof(NsApplicationControlData), out_size);
}
}  // namespace

bool Titles(const Request& req, Reply& reply) {
    (void)req;
    auto control = std::make_unique<NsApplicationControlData>();
    json::Value list = json::Value::array();

    NsApplicationRecord records[32];
    s32 offset = 0;
    while (true) {
        s32 got = 0;
        Result rc = nsListApplicationRecord(records, 32, offset, &got);
        if (R_FAILED(rc) || got == 0) break;
        for (s32 i = 0; i < got; i++) {
            u64 tid = records[i].application_id;
            json::Value t = json::Value::object();
            char hex[17];
            TidToHex(tid, hex);
            t.set("tid", hex);

            u64 sz = 0;
            if (R_SUCCEEDED(LoadControl(tid, control.get(), &sz))) {
                NacpLanguageEntry* entry = nullptr;
                nacpGetLanguageEntry(&control->nacp, &entry);
                // nacpGetLanguageEntry yields NULL/empty for the system language
                // on some titles; fall back to the first non-empty language.
                if (!entry || entry->name[0] == '\0') {
                    for (int l = 0; l < 16; l++) {
                        if (control->nacp.lang[l].name[0] != '\0') {
                            entry = &control->nacp.lang[l];
                            break;
                        }
                    }
                }
                if (entry && entry->name[0] != '\0') {
                    t.set("name", entry->name);
                    t.set("author", entry->author);
                }
                t.set("version", control->nacp.display_version);
            }
            list.push(std::move(t));
        }
        offset += got;
        if (got < 32) break;
    }
    reply.json.set("titles", std::move(list));
    return true;
}

bool TitleIcon(const Request& req, Reply& reply) {
    u64 tid = std::strtoull(req["tid"].as_string("0").c_str(), nullptr, 16);
    if (!tid) return Fail(reply, "bad_arg", "missing tid");
    auto control = std::make_unique<NsApplicationControlData>();
    u64 sz = 0;
    if (R_FAILED(LoadControl(tid, control.get(), &sz)))
        return Fail(reply, "not_found", "no control data for title");
    // Icon is the JPEG trailing the NACP; its size is total - nacp offset.
    size_t icon_size = sz > sizeof(control->nacp) ? sz - sizeof(control->nacp) : 0;
    if (icon_size == 0) return Fail(reply, "not_found", "no icon");
    reply.out.assign(control->icon, control->icon + icon_size);
    reply.json.set("jpeg", true);
    return true;
}

bool RunningApp(const Request& req, Reply& reply) {
    (void)req;
    u64 pid = 0;
    Result rc = pmdmntGetApplicationProcessId(&pid);
    if (R_FAILED(rc) || pid == 0) {
        reply.json.set("running", false);
        return true;
    }
    reply.json.set("running", true);
    reply.json.set("pid", (int64_t)pid);
    u64 tid = 0;
    if (R_SUCCEEDED(pminfoGetProgramId(&tid, pid))) {
        char hex[17];
        TidToHex(tid, hex);
        reply.json.set("tid", hex);
    }
    return true;
}

bool Launch(const Request& req, Reply& reply) {
    // hbloader launched by title ID starts a process that never renders, and
    // the dead process then holds the application slot so the normal album
    // takeover also fails. Refuse rather than leave the console in a state that
    // looks like "homebrew is broken". Verified on hardware.
    if (req["tid"].as_string() == "0142b048fd620000" && !req["force"].as_bool(false))
        return Fail(reply, "use_album_applet",
                    "0142b048fd620000 is the hbloader takeover ID. Launching it "
                    "directly spawns a process that renders nothing and then "
                    "blocks the real launch path. Start homebrew via the Album "
                    "applet, or hold R while launching a game. Pass force=true "
                    "if you really mean it.");

    u64 tid = std::strtoull(req["tid"].as_string("0").c_str(), nullptr, 16);
    if (!tid) return Fail(reply, "bad_arg", "missing tid");

    // Try the common storage locations in turn; installed titles live on either
    // the internal user partition or the SD card.
    static const NcmStorageId kStorages[] = {NcmStorageId_BuiltInUser,
                                             NcmStorageId_SdCard, NcmStorageId_Any};
    Result rc = 1;
    u64 pid = 0;
    for (NcmStorageId storage : kStorages) {
        NcmProgramLocation loc = {.program_id = tid, .storageID = (u8)storage};
        rc = pmshellLaunchProgram(0, &loc, &pid);
        if (R_SUCCEEDED(rc)) break;
    }
    if (R_FAILED(rc)) return Fail(reply, "launch_failed", "pmshellLaunchProgram failed");
    reply.json.set("ok", true);
    reply.json.set("pid", (int64_t)pid);
    return true;
}

bool Terminate(const Request& req, Reply& reply) {
    u64 tid = 0;
    if (req.msg.has("tid"))
        tid = std::strtoull(req["tid"].as_string("0").c_str(), nullptr, 16);

    // No tid → terminate the current foreground application.
    if (tid == 0) {
        u64 pid = 0;
        if (R_FAILED(pmdmntGetApplicationProcessId(&pid)) || pid == 0)
            return Fail(reply, "no_app", "no foreground application running");
        if (R_FAILED(pminfoGetProgramId(&tid, pid)))
            return Fail(reply, "no_app", "cannot resolve foreground title");
    }
    Result rc = pmshellTerminateProgram(tid);
    if (R_FAILED(rc)) return Fail(reply, "terminate_failed", "pmshellTerminateProgram failed");
    reply.json.set("ok", true);
    return true;
}

// Uninstall a title. DESTRUCTIVE and gated by config (allow_nand_write) since it
// permanently deletes the installed application. Save data is left intact.
// NOTE: installing an NSP is intentionally NOT implemented — it duplicates
// DBI/Awoo Installer (PFS0 parsing + ncm content import, ~hundreds of lines of
// bricky code) with no advantage here. Use those for installs; this manages
// what's already on the console.
bool Uninstall(const Request& req, Reply& reply) {
    if (!req.cfg.allow_nand_write)
        return Fail(reply, "disabled", "uninstall requires allow_nand_write=true in config");
    u64 tid = std::strtoull(req["tid"].as_string("0").c_str(), nullptr, 16);
    if (!tid) return Fail(reply, "bad_arg", "missing tid");
    Result rc = nsDeleteApplicationCompletely(tid);
    if (R_FAILED(rc)) return Fail(reply, "uninstall_failed", "nsDeleteApplicationCompletely failed");
    reply.json.set("ok", true);
    return true;
}

}  // namespace handlers
}  // namespace agent
