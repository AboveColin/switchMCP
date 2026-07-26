// Live memory inspection and iterative narrowing search, via dmnt:cht.
//
// Two things the debugger path in debug.cpp fundamentally cannot do:
//
// 1. Read a RUNNING process. svcDebugActiveProcess stops the target, so every
//    read there is of a frozen game. Values that only exist while the game is
//    moving — a timer counting down, a health bar draining — are invisible.
//    Atmosphère's cheat service reads the attached cheat process without
//    pausing it, which is exactly the primitive value-hunting needs.
//
// 2. Converge. debug.search is one-shot: it returns every address holding a
//    value, which for something like 100 is tens of thousands of hits and tells
//    you nothing. The way this is actually done is a first scan followed by
//    repeated filtering — "now it decreased", "now it is 97" — until a handful
//    of addresses remain. That requires keeping candidates between requests,
//    which is why the table lives on the device: shipping a million addresses
//    to the client and back per step would be absurd.
//
// libnx has no dmnt:cht wrapper, so this speaks the raw IPC, matching the
// existing style in cheat.cpp.
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

Service g_svc;
bool g_open = false;

// Atmosphère dmnt:cht command IDs.
enum {
    Cmd_HasCheatProcess = 65000,
    Cmd_GetCheatProcessMetadata = 65002,
    Cmd_ForceOpenCheatProcess = 65003,
    Cmd_GetCheatProcessMappingCount = 65100,
    Cmd_GetCheatProcessMappings = 65101,
    Cmd_ReadCheatProcessMemory = 65102,
    Cmd_WriteCheatProcessMemory = 65103,
};

struct MemoryRegionExtents {
    u64 base;
    u64 size;
};

struct CheatProcessMetadata {
    u64 process_id;
    u64 title_id;
    MemoryRegionExtents main_nso;
    MemoryRegionExtents heap;
    MemoryRegionExtents alias;
    MemoryRegionExtents address_space;
    u8 main_nso_build_id[0x20];
};

bool Open(Reply& reply) {
    if (g_open) return true;
    if (R_FAILED(smGetService(&g_svc, "dmnt:cht"))) {
        Fail(reply, "no_service",
             "dmnt:cht unavailable. Atmosphère's cheat module must be enabled "
             "(dmnt_cheats_enabled_by_default, or a cheat process force-opened).");
        return false;
    }
    g_open = true;
    return true;
}

// Make sure a cheat process is attached, force-opening the foreground
// application if not. Without this every call fails on a console that simply
// has not had a cheat process opened yet.
bool EnsureCheatProcess(Reply& reply) {
    if (!Open(reply)) return false;
    u8 has = 0;
    if (R_FAILED(serviceDispatchOut(&g_svc, Cmd_HasCheatProcess, has)))
        return Fail(reply, "ipc_failed", "HasCheatProcess failed");
    if (has) return true;

    u64 pid = 0;
    if (R_FAILED(pmdmntGetApplicationProcessId(&pid)) || pid == 0)
        return Fail(reply, "no_app",
                    "no cheat process and no foreground application to open one on");
    if (R_FAILED(serviceDispatchIn(&g_svc, Cmd_ForceOpenCheatProcess, pid)))
        return Fail(reply, "open_failed",
                    "ForceOpenCheatProcess failed for the foreground application");
    return true;
}

bool ReadMem(u64 addr, void* out, size_t size) {
    const struct {
        u64 address;
        u64 size;
    } in = {addr, size};
    return R_SUCCEEDED(serviceDispatchIn(
        &g_svc, Cmd_ReadCheatProcessMemory, in,
        .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
        .buffers = {{out, size}}));
}

bool WriteMem(u64 addr, const void* data, size_t size) {
    const struct {
        u64 address;
        u64 size;
    } in = {addr, size};
    return R_SUCCEEDED(serviceDispatchIn(
        &g_svc, Cmd_WriteCheatProcessMemory, in,
        .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_In},
        .buffers = {{const_cast<void*>(data), size}}));
}

bool GetMeta(CheatProcessMetadata& out) {
    return R_SUCCEEDED(serviceDispatchOut(&g_svc, Cmd_GetCheatProcessMetadata, out));
}

// Enumerate the process's actual memory mappings.
//
// Necessary, not a nicety: the heap *extent* in the metadata is a reserved
// 8 GiB address range, almost all of it unmapped. Scanning it blindly means
// ~131000 failing IPC calls to cover a region whose live part is a few hundred
// MiB. Walking real mappings turns a search from impractical into seconds.
bool GetMappings(std::vector<MemoryInfo>& out) {
    u64 count = 0;
    if (R_FAILED(serviceDispatchOut(&g_svc, Cmd_GetCheatProcessMappingCount, count)))
        return false;
    if (count == 0 || count > 4096) return false;

    out.resize((size_t)count);
    u64 offset = 0, got = 0;
    Result rc = serviceDispatchInOut(
        &g_svc, Cmd_GetCheatProcessMappings, offset, got,
        .buffer_attrs = {SfBufferAttr_HipcMapAlias | SfBufferAttr_Out},
        .buffers = {{out.data(), out.size() * sizeof(MemoryInfo)}});
    if (R_FAILED(rc)) return false;
    out.resize((size_t)(got < count ? got : count));
    return true;
}

// Regions worth scanning for game state: readable, writable, and actually
// backed. Code and read-only data cannot hold a changing value, so including
// them only wastes time and inflates the candidate set.
bool ScannableRegion(const MemoryInfo& mi) {
    u32 type = mi.type & 0xFF;
    if (!(mi.perm & Perm_R) || !(mi.perm & Perm_W)) return false;
    return type == MemType_Heap || type == MemType_Normal ||
           type == MemType_CodeMutable || type == MemType_ModuleCodeMutable;
}

// --- narrowing search state --------------------------------------------------

// Capped so a first scan for a very common value cannot exhaust the heap. The
// cap is reported to the caller rather than silently applied: a truncated
// candidate set that looks complete would send someone chasing the wrong
// address.
// Must stay in step with kMemSearchBudget in main.cpp. Kept deliberately small:
// a 16 MiB heap to allow a larger table prevented the sysmodule from booting.
constexpr size_t kMaxCandidates = 32768;
constexpr size_t kScanChunk = 64 * 1024;

struct Candidate {
    u64 addr;
    u64 value;   // value at the last scan, for change-relative operators
};

std::vector<Candidate> g_candidates;
int g_width = 4;
int g_generation = 0;

u64 LoadValue(const u8* p, int width) {
    u64 v = 0;
    std::memcpy(&v, p, (size_t)width);
    return v;
}

}  // namespace

// Read memory of the running, unpaused cheat process.
bool CheatReadMem(const Request& req, Reply& reply) {
    if (!EnsureCheatProcess(reply)) return false;
    u64 addr = (u64)req["addr"].as_int(0);
    int len = (int)req["len"].as_int(256);
    if (len < 1 || len > (1 << 20)) len = 256;
    if (!addr) return Fail(reply, "bad_arg", "missing addr");

    reply.out.resize((size_t)len);
    if (!ReadMem(addr, reply.out.data(), (size_t)len)) {
        reply.out.clear();
        return Fail(reply, "read_failed",
                    "ReadCheatProcessMemory failed (address not mapped?)");
    }
    reply.json.set("addr", (int64_t)addr);
    reply.json.set("len", (int64_t)len);
    reply.json.set("paused", false);
    return true;
}

// Write memory of the running process. Invasive: patches a live game.
bool CheatWriteMem(const Request& req, Reply& reply) {
    if (!EnsureCheatProcess(reply)) return false;
    u64 addr = (u64)req["addr"].as_int(0);
    if (!addr) return Fail(reply, "bad_arg", "missing addr");
    if (req.payload.empty()) return Fail(reply, "bad_arg", "no payload to write");
    if (!WriteMem(addr, req.payload.data(), req.payload.size()))
        return Fail(reply, "write_failed", "WriteCheatProcessMemory failed");
    reply.json.set("written", (int64_t)req.payload.size());
    reply.json.set("addr", (int64_t)addr);
    return true;
}

// Title, main-module extents, heap extents and build ID — without attaching a
// debugger and without pausing anything.
bool CheatMeta(const Request& req, Reply& reply) {
    (void)req;
    if (!EnsureCheatProcess(reply)) return false;
    CheatProcessMetadata m{};
    if (!GetMeta(m)) return Fail(reply, "ipc_failed", "GetCheatProcessMetadata failed");

    char hx[19];
    auto region = [&](const char* name, const MemoryRegionExtents& e) {
        json::Value r = json::Value::object();
        std::snprintf(hx, sizeof(hx), "0x%lx", (unsigned long)e.base);
        r.set("base", hx);
        std::snprintf(hx, sizeof(hx), "0x%lx", (unsigned long)e.size);
        r.set("size", hx);
        reply.json.set(name, std::move(r));
    };
    reply.json.set("process_id", (int64_t)m.process_id);
    std::snprintf(hx, sizeof(hx), "%016lx", (unsigned long)m.title_id);
    reply.json.set("title_id", hx);
    region("main_nso", m.main_nso);
    region("heap", m.heap);
    region("alias", m.alias);
    region("address_space", m.address_space);
    char bid[0x41];
    for (int i = 0; i < 0x20; i++) std::snprintf(bid + i * 2, 3, "%02x", m.main_nso_build_id[i]);
    reply.json.set("main_nso_build_id", bid);
    return true;
}

// The process's real memory mappings.
bool CheatMappings(const Request& req, Reply& reply) {
    if (!EnsureCheatProcess(reply)) return false;
    int limit = (int)req["limit"].as_int(60);
    if (limit < 1 || limit > 500) limit = 60;

    std::vector<MemoryInfo> maps;
    if (!GetMappings(maps))
        return Fail(reply, "ipc_failed", "GetCheatProcessMappings failed");

    json::Value arr = json::Value::array();
    char hx[19];
    int shown = 0, scannable = 0;
    u64 total_rw = 0;
    for (const MemoryInfo& mi : maps) {
        bool ok = ScannableRegion(mi);
        if (ok) { scannable++; total_rw += mi.size; }
        if (shown >= limit) continue;
        shown++;
        json::Value e = json::Value::object();
        std::snprintf(hx, sizeof(hx), "0x%lx", (unsigned long)mi.addr);
        e.set("addr", hx);
        std::snprintf(hx, sizeof(hx), "0x%lx", (unsigned long)mi.size);
        e.set("size", hx);
        char perm[4];
        perm[0] = (mi.perm & Perm_R) ? 'r' : '-';
        perm[1] = (mi.perm & Perm_W) ? 'w' : '-';
        perm[2] = (mi.perm & Perm_X) ? 'x' : '-';
        perm[3] = 0;
        e.set("perm", perm);
        e.set("type", (int64_t)(mi.type & 0xFF));
        e.set("scannable", ok);
        arr.push(std::move(e));
    }
    reply.json.set("total_mappings", (int64_t)maps.size());
    reply.json.set("scannable_mappings", (int64_t)scannable);
    reply.json.set("scannable_bytes", (int64_t)total_rw);
    reply.json.set("shown", (int64_t)shown);
    reply.json.set("mappings", std::move(arr));
    return true;
}

// First scan: collect every address in a region holding `value`.
bool SearchBegin(const Request& req, Reply& reply) {
    if (!EnsureCheatProcess(reply)) return false;

    int width = (int)req["width"].as_int(4);
    if (width != 1 && width != 2 && width != 4 && width != 8)
        return Fail(reply, "bad_arg", "width must be 1, 2, 4 or 8");
    u64 want_value = (u64)req["value"].as_int(0);

    CheatProcessMetadata m{};
    if (!GetMeta(m)) return Fail(reply, "ipc_failed", "GetCheatProcessMetadata failed");

    // Explicit base/size wins; otherwise walk the real mappings.
    std::vector<std::pair<u64, u64>> regions;
    if (req.msg.has("base") && req.msg.has("size")) {
        regions.push_back({(u64)req["base"].as_int(0), (u64)req["size"].as_int(0)});
    } else {
        std::vector<MemoryInfo> maps;
        if (!GetMappings(maps))
            return Fail(reply, "ipc_failed", "GetCheatProcessMappings failed");
        std::string want = req["region"].as_string("");
        for (const MemoryInfo& mi : maps) {
            if (!ScannableRegion(mi)) continue;
            // Optional narrowing to one of the named extents.
            if (want == "heap" &&
                !(mi.addr >= m.heap.base && mi.addr < m.heap.base + m.heap.size))
                continue;
            if (want == "main" &&
                !(mi.addr >= m.main_nso.base &&
                  mi.addr < m.main_nso.base + m.main_nso.size))
                continue;
            regions.push_back({mi.addr, mi.size});
        }
        if (regions.empty())
            return Fail(reply, "no_regions",
                        "no readable+writable mappings matched; try region=\"\" "
                        "to scan everything writable");
    }

    // Wall-clock guard. A big game can map well over a GiB of writable memory
    // and each 64 KiB chunk is an IPC round-trip, so an unbounded scan would
    // hold the single service thread for minutes and look like a hang.
    u64 budget = (u64)req["max_bytes"].as_int(512ll * 1024 * 1024);

    g_candidates.clear();
    g_width = width;
    g_generation = 1;

    std::vector<u8> buf(kScanChunk);
    bool truncated = false, budget_hit = false;
    u64 scanned = 0;
    for (const auto& reg : regions) {
        if (truncated || budget_hit) break;
        for (u64 off = 0; off < reg.second; off += kScanChunk) {
            if (scanned >= budget) { budget_hit = true; break; }
            size_t chunk = (size_t)((reg.second - off) < kScanChunk ? (reg.second - off)
                                                                    : kScanChunk);
            if (!ReadMem(reg.first + off, buf.data(), chunk)) continue;
            scanned += chunk;
            for (size_t i = 0; i + (size_t)width <= chunk; i += (size_t)width) {
                if (LoadValue(&buf[i], width) == want_value) {
                    if (g_candidates.size() >= kMaxCandidates) { truncated = true; break; }
                    g_candidates.push_back({reg.first + off + i, want_value});
                }
            }
            if (truncated) break;
        }
    }
    reply.json.set("regions_scanned", (int64_t)regions.size());
    if (budget_hit) {
        reply.json.set("budget_exhausted", true);
        reply.json.set("budget_note",
                       "stopped at max_bytes; raise it or narrow with region= if "
                       "the address you want was not reached");
    }

    LOG_INFO("search: first scan value=%llu width=%d -> %zu candidates",
             (unsigned long long)want_value, width, g_candidates.size());
    reply.json.set("generation", (int64_t)g_generation);
    reply.json.set("candidates", (int64_t)g_candidates.size());
    reply.json.set("bytes_scanned", (int64_t)scanned);
    reply.json.set("width", (int64_t)width);
    if (truncated) {
        reply.json.set("truncated", true);
        reply.json.set("note",
                       "hit the candidate cap. Narrow with search.next, or pick a "
                       "rarer starting value — a truncated set may not contain the "
                       "address you want.");
    }
    return true;
}

// Filter the surviving candidates against a new observation.
bool SearchNext(const Request& req, Reply& reply) {
    if (!EnsureCheatProcess(reply)) return false;
    if (g_candidates.empty())
        return Fail(reply, "no_search",
                    "no active search; call search.begin first (or the previous "
                    "scan eliminated every candidate)");

    std::string op = req["op"].as_string("eq");
    u64 want = (u64)req["value"].as_int(0);
    bool needs_value = (op == "eq" || op == "ne" || op == "gt" || op == "lt");
    if (needs_value && !req.msg.has("value"))
        return Fail(reply, "bad_arg", op + " needs a 'value'");

    std::vector<Candidate> kept;
    kept.reserve(g_candidates.size());
    u8 raw[8];
    for (const Candidate& c : g_candidates) {
        if (!ReadMem(c.addr, raw, (size_t)g_width)) continue;  // vanished: drop
        u64 now = LoadValue(raw, g_width);
        bool keep;
        if (op == "eq") keep = now == want;
        else if (op == "ne") keep = now != want;
        else if (op == "gt") keep = now > want;
        else if (op == "lt") keep = now < want;
        else if (op == "changed") keep = now != c.value;
        else if (op == "unchanged") keep = now == c.value;
        else if (op == "increased") keep = now > c.value;
        else if (op == "decreased") keep = now < c.value;
        else return Fail(reply, "bad_arg",
                         "op must be eq, ne, gt, lt, changed, unchanged, "
                         "increased or decreased");
        if (keep) kept.push_back({c.addr, now});
    }

    size_t before = g_candidates.size();
    g_candidates.swap(kept);
    g_generation++;
    LOG_INFO("search: %s -> %zu of %zu candidates survive", op.c_str(),
             g_candidates.size(), before);

    reply.json.set("generation", (int64_t)g_generation);
    reply.json.set("op", op);
    reply.json.set("before", (int64_t)before);
    reply.json.set("candidates", (int64_t)g_candidates.size());
    if (g_candidates.empty())
        reply.json.set("note",
                       "every candidate was eliminated — the value probably is not "
                       "this width, or the observation was wrong. Start over.");
    return true;
}

// Current candidates with their latest values.
bool SearchResults(const Request& req, Reply& reply) {
    int limit = (int)req["limit"].as_int(50);
    if (limit < 1 || limit > 1000) limit = 50;
    json::Value arr = json::Value::array();
    char hx[19];
    int n = 0;
    for (const Candidate& c : g_candidates) {
        if (n++ >= limit) break;
        json::Value e = json::Value::object();
        std::snprintf(hx, sizeof(hx), "0x%lx", (unsigned long)c.addr);
        e.set("addr", hx);
        e.set("value", (int64_t)c.value);
        arr.push(std::move(e));
    }
    reply.json.set("generation", (int64_t)g_generation);
    reply.json.set("total", (int64_t)g_candidates.size());
    reply.json.set("width", (int64_t)g_width);
    reply.json.set("shown", (int64_t)n);
    reply.json.set("results", std::move(arr));
    return true;
}

bool SearchReset(const Request& req, Reply& reply) {
    (void)req;
    g_candidates.clear();
    g_candidates.shrink_to_fit();   // hand the pages back; this can be MiBs
    g_generation = 0;
    reply.json.set("ok", true);
    return true;
}

}  // namespace handlers
}  // namespace agent
