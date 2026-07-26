// Atmosphère cheat engine (dmnt:cht) via raw IPC — libnx has no wrapper for it.
// Command IDs are from Atmosphère's dmnt cheat service. Complements the raw
// memory search/patch in debug.cpp with the cheat-code VM + value freezing.
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
Service g_cht;
bool g_cht_open = false;

bool OpenCht(Reply& reply) {
    if (g_cht_open) return true;
    if (R_FAILED(smGetService(&g_cht, "dmnt:cht"))) {
        Fail(reply, "no_service", "dmnt:cht unavailable (cheat module off?)");
        return false;
    }
    g_cht_open = true;
    return true;
}

// Atmosphère CheatEntry layout (readable_name[0x40] + num_opcodes + opcodes).
struct CheatDefinition {
    char readable_name[0x40];
    uint32_t num_opcodes;
    uint32_t opcodes[0x100];
};
struct CheatEntry {
    uint32_t enabled;
    uint32_t cheat_id;
    CheatDefinition definition;
};
}  // namespace

bool CheatStatus(const Request& req, Reply& reply) {
    (void)req;
    if (!OpenCht(reply)) return false;
    u8 has = 0;
    if (R_FAILED(serviceDispatchOut(&g_cht, 65000, has)))  // HasCheatProcess
        return Fail(reply, "ipc_failed", "HasCheatProcess failed");
    reply.json.set("has_cheat_process", (bool)has);
    if (has) {
        u64 count = 0;
        if (R_SUCCEEDED(serviceDispatchOut(&g_cht, 65100, count)))  // GetCheatCount
            reply.json.set("cheat_count", (int64_t)count);
        u64 frozen = 0;
        if (R_SUCCEEDED(serviceDispatchOut(&g_cht, 65200, frozen)))  // GetFrozenAddressCount
            reply.json.set("frozen_count", (int64_t)frozen);
    }
    return true;
}

bool CheatList(const Request& req, Reply& reply) {
    (void)req;
    if (!OpenCht(reply)) return false;
    u64 total = 0;
    if (R_FAILED(serviceDispatchOut(&g_cht, 65100, total)))
        return Fail(reply, "ipc_failed", "GetCheatCount failed");
    if (total > 128) total = 128;
    std::vector<CheatEntry> entries(total ? total : 1);
    struct {
        u64 count;
        u64 offset;
    } in = {total, 0};
    u64 out_count = 0;
    // GetCheats: In(count, offset), Out(count), OutBuffer(entries).
    Result rc = serviceDispatchInOut(
        &g_cht, 65101, in, out_count,
        .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
        .buffers = {{entries.data(), entries.size() * sizeof(CheatEntry)}});
    if (R_FAILED(rc)) return Fail(reply, "ipc_failed", "GetCheats failed");
    json::Value list = json::Value::array();
    for (u64 i = 0; i < out_count && i < entries.size(); i++) {
        json::Value e = json::Value::object();
        e.set("cheat_id", (int64_t)entries[i].cheat_id);
        e.set("enabled", (bool)entries[i].enabled);
        entries[i].definition.readable_name[0x3F] = 0;
        e.set("name", entries[i].definition.readable_name);
        e.set("opcode_count", (int64_t)entries[i].definition.num_opcodes);
        list.push(std::move(e));
    }
    reply.json.set("cheats", std::move(list));
    return true;
}

bool CheatToggle(const Request& req, Reply& reply) {
    if (!OpenCht(reply)) return false;
    u32 id = (u32)req["cheat_id"].as_int(-1);
    if (R_FAILED(serviceDispatchIn(&g_cht, 65103, id)))  // ToggleCheat
        return Fail(reply, "ipc_failed", "ToggleCheat failed");
    reply.json.set("ok", true);
    reply.json.set("cheat_id", (int64_t)id);
    return true;
}

bool FreezeAddress(const Request& req, Reply& reply) {
    if (!OpenCht(reply)) return false;
    struct {
        u64 address;
        u64 width;
    } in = {(u64)req["addr"].as_int(0), (u64)req["width"].as_int(4)};
    u64 value = 0;
    // EnableFrozenAddress: In(address, width), Out(current value).
    if (R_FAILED(serviceDispatchInOut(&g_cht, 65203, in, value)))
        return Fail(reply, "ipc_failed", "EnableFrozenAddress failed");
    reply.json.set("ok", true);
    reply.json.set("frozen_value", (int64_t)value);
    return true;
}

bool UnfreezeAddress(const Request& req, Reply& reply) {
    if (!OpenCht(reply)) return false;
    u64 addr = (u64)req["addr"].as_int(0);
    if (R_FAILED(serviceDispatchIn(&g_cht, 65204, addr)))  // DisableFrozenAddress
        return Fail(reply, "ipc_failed", "DisableFrozenAddress failed");
    reply.json.set("ok", true);
    return true;
}

}  // namespace handlers
}  // namespace agent
