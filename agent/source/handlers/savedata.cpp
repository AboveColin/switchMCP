// Save-data enumeration and read-only browsing.
//
// Until now the only way to see a save was `save.backup`, which copies the
// whole thing to the SD card first. That is slow, needs free space, and tells
// you nothing about saves you have not already guessed the title ID for.
//
// fsOpenSaveDataInfoReader enumerates *every* save on the console — game saves
// per user, system saves, BCAT, device and cache storage — with sizes and
// owners. Combined with a read-only mount, a save can then be browsed and read
// in place.
//
// Everything here is read-only on purpose. Mounts go through
// fsdevMountSaveDataReadOnly, so a mistake cannot corrupt save data, and there
// is no backup to fall back on.
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

const char* SaveTypeName(u8 t) {
    switch (t) {
        case FsSaveDataType_System: return "system";
        case FsSaveDataType_Account: return "account";
        case FsSaveDataType_Bcat: return "bcat";
        case FsSaveDataType_Device: return "device";
        case FsSaveDataType_Temporary: return "temporary";
        case FsSaveDataType_Cache: return "cache";
        case FsSaveDataType_SystemBcat: return "system_bcat";
    }
    return "unknown";
}

const char* SpaceName(u8 s) {
    switch (s) {
        case FsSaveDataSpaceId_System: return "system";
        case FsSaveDataSpaceId_User: return "user";
        case FsSaveDataSpaceId_SdSystem: return "sd_system";
        case FsSaveDataSpaceId_Temporary: return "temporary";
        case FsSaveDataSpaceId_SdUser: return "sd_user";
        case FsSaveDataSpaceId_ProperSystem: return "proper_system";
        case FsSaveDataSpaceId_SafeMode: return "safe_mode";
    }
    return "unknown";
}

void SetHex64(json::Value& obj, const char* key, u64 v) {
    char buf[19];
    std::snprintf(buf, sizeof(buf), "%016lx", (unsigned long)v);
    obj.set(key, buf);
}

}  // namespace

// Enumerate every save on the console.
bool SaveList(const Request& req, Reply& reply) {
    std::string want_type = req["type"].as_string();   // optional filter
    int limit = (int)req["limit"].as_int(400);
    if (limit < 1 || limit > 2000) limit = 400;

    FsSaveDataInfoReader reader;
    // SpaceId_All asks for everything rather than one storage class.
    Result rc = fsOpenSaveDataInfoReader(&reader, (FsSaveDataSpaceId)FsSaveDataSpaceId_All);
    if (R_FAILED(rc))
        return Fail(reply, "fs_failed", "cannot open the save-data info reader");

    json::Value arr = json::Value::array();
    int total = 0, listed = 0;
    // Read in batches; the reader is a cursor, not a snapshot we can index.
    std::vector<FsSaveDataInfo> batch(16);
    while (true) {
        s64 got = 0;
        if (R_FAILED(fsSaveDataInfoReaderRead(&reader, batch.data(), batch.size(), &got)))
            break;
        if (got <= 0) break;
        for (s64 i = 0; i < got; i++) {
            const FsSaveDataInfo& s = batch[i];
            total++;
            const char* type = SaveTypeName(s.save_data_type);
            if (!want_type.empty() && want_type != type) continue;
            if (listed >= limit) continue;
            listed++;

            json::Value e = json::Value::object();
            SetHex64(e, "save_data_id", s.save_data_id);
            e.set("type", type);
            e.set("space", SpaceName(s.save_data_space_id));
            if (s.application_id) SetHex64(e, "application_id", s.application_id);
            if (s.system_save_data_id)
                SetHex64(e, "system_save_data_id", s.system_save_data_id);
            e.set("size", (int64_t)s.size);
            e.set("index", (int64_t)s.save_data_index);
            // A zero uid means common/system save rather than a user's.
            if (s.uid.uid[0] || s.uid.uid[1]) {
                SetHex64(e, "uid_hi", s.uid.uid[0]);
                SetHex64(e, "uid_lo", s.uid.uid[1]);
            }
            arr.push(std::move(e));
        }
    }
    fsSaveDataInfoReaderClose(&reader);

    reply.json.set("total", (int64_t)total);
    reply.json.set("listed", (int64_t)listed);
    if (listed < total && want_type.empty())
        reply.json.set("truncated", true);
    reply.json.set("saves", std::move(arr));
    return true;
}

// Mount a save read-only so it can be browsed with fs.list / fs.read using
// device="save". One mount at a time, which keeps the device-name space
// predictable and means an abandoned session cannot leak mounts.
bool SaveMount(const Request& req, Reply& reply) {
    // Re-mounting is fine; drop any previous one first.
    fsdevUnmountDevice(kSaveMountName);

    u64 tid = std::strtoull(req["tid"].as_string("0").c_str(), nullptr, 16);
    u64 sys_id = std::strtoull(req["system_save_data_id"].as_string("0").c_str(), nullptr, 16);
    AccountUid uid{};
    uid.uid[0] = (u64)std::strtoull(req["uid_hi"].as_string("0").c_str(), nullptr, 16);
    uid.uid[1] = (u64)std::strtoull(req["uid_lo"].as_string("0").c_str(), nullptr, 16);

    Result rc;
    if (sys_id) {
        // System saves are addressed by their own id, not an application id,
        // and they do not all live in the same space: save.list reports the
        // space per entry, so honour it and only guess as a fallback.
        std::string space = req["space"].as_string();
        FsSaveDataSpaceId spaces[3];
        int n = 0;
        if (space == "system") spaces[n++] = FsSaveDataSpaceId_System;
        else if (space == "sd_system") spaces[n++] = FsSaveDataSpaceId_SdSystem;
        else if (space == "proper_system") spaces[n++] = FsSaveDataSpaceId_ProperSystem;
        if (n == 0) {  // unspecified: try the plausible ones in order
            spaces[n++] = FsSaveDataSpaceId_System;
            spaces[n++] = FsSaveDataSpaceId_SdSystem;
            spaces[n++] = FsSaveDataSpaceId_ProperSystem;
        }
        rc = MAKERESULT(Module_Libnx, LibnxError_NotFound);
        for (int i = 0; i < n; i++) {
            rc = fsdevMountSystemSaveData(kSaveMountName, spaces[i], sys_id, uid);
            if (R_SUCCEEDED(rc)) break;
        }
    } else {
        if (!tid) return Fail(reply, "bad_arg", "need tid or system_save_data_id");
        rc = fsdevMountSaveDataReadOnly(kSaveMountName, tid, uid);
    }

    if (R_FAILED(rc)) {
        char m[256];
        std::snprintf(m, sizeof(m),
                      "cannot mount that save (rc=0x%08x). Common causes: the "
                      "system already holds it open — settings, account and "
                      "similar system saves are mounted by their owning service "
                      "and cannot be opened twice; a wrong uid for an account "
                      "save (take it from save.list); or the title simply has no "
                      "save data.",
                      (unsigned)rc);
        return Fail(reply, "mount_failed", m);
    }

    g_save_mounted = true;
    LOG_INFO("save mounted read-only: tid=%016lx sys=%016lx", (unsigned long)tid,
             (unsigned long)sys_id);
    reply.json.set("ok", true);
    reply.json.set("device", "save");
    reply.json.set("mode", "read-only");
    reply.json.set("note", "browse with fs.list / fs.read using device=\"save\"");
    return true;
}

bool SaveUnmount(const Request& req, Reply& reply) {
    (void)req;
    fsdevUnmountDevice(kSaveMountName);
    g_save_mounted = false;
    reply.json.set("ok", true);
    return true;
}

}  // namespace handlers
}  // namespace agent
