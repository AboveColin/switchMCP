// TCP server: accept loop, frame codec, auth, command dispatch.
#include "protocol.hpp"

#include <switch.h>

#include <arpa/inet.h>
#include <cstring>
#include <errno.h>
#include <map>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include "log.hpp"

namespace agent {

namespace {

constexpr uint32_t kMaxJson = 64 * 1024;
constexpr uint64_t kMaxBinary = 1u << 20;  // 1 MiB, matches transfer chunk

// Command table: handler, the authority it needs, and whether it mutates.
//
// Tier assignment is by blast radius, not by how the command feels:
//   Observe  - cannot change the console at all
//   Control  - does what a person holding the console could do
//   Invasive - can leave it unbootable, damage hardware, or destroy data
//
// `mutating` drives dry_run support: the dispatcher answers those with a
// preview instead of running them when the caller asks.
const std::map<std::string, Command>& Dispatch() {
    static const std::map<std::string, Command> table = {
        // --- observe: read-only -------------------------------------------
        {"agent.info",        {handlers::AgentInfo,        Tier::Observe,  false}},
        {"screenshot",        {handlers::Screenshot,       Tier::Observe,  false}},
        {"sysinfo",           {handlers::SysInfo,          Tier::Observe,  false}},
        {"ps",                {handlers::ProcessList,      Tier::Observe,  false}},
        {"crash_reports",     {handlers::CrashReports,     Tier::Observe,  false}},
        {"read_log",          {handlers::ReadLog,          Tier::Observe,  false}},
        {"titles",            {handlers::Titles,           Tier::Observe,  false}},
        {"title_icon",        {handlers::TitleIcon,        Tier::Observe,  false}},
        {"running_app",       {handlers::RunningApp,       Tier::Observe,  false}},
        {"console_info",      {handlers::ConsoleInfo,      Tier::Observe,  false}},
        {"net_info",          {handlers::NetInfo,          Tier::Observe,  false}},
        {"fatal_reports",     {handlers::FatalReports,     Tier::Observe,  false}},
        {"preflight",         {handlers::PreflightCheck,   Tier::Observe,  false}},
        {"journal",           {handlers::JournalRead,      Tier::Observe,  false}},
        {"watchdog",          {handlers::WatchdogStatus,   Tier::Observe,  false}},
        {"controllers",       {handlers::Controllers,      Tier::Observe,  false}},
        {"read_input",        {handlers::ReadInput,        Tier::Observe,  false}},
        {"wait_event",        {handlers::WaitEvent,        Tier::Observe,  false}},
        {"record_input",      {handlers::RecordInput,      Tier::Observe,  false}},
        {"get_clocks",        {handlers::GetClocks,        Tier::Observe,  false}},
        {"get_brightness",    {handlers::GetBrightness,    Tier::Observe,  false}},
        {"get_volume",        {handlers::GetVolume,        Tier::Observe,  false}},
        {"get_time",          {handlers::GetTime,          Tier::Observe,  false}},
        {"get_sleep",         {handlers::GetSleep,         Tier::Observe,  false}},
        {"settings.get",      {handlers::SettingsGet,      Tier::Observe,  false}},
        {"fs.list",           {handlers::FsList,           Tier::Observe,  false}},
        {"fs.read",           {handlers::FsRead,           Tier::Observe,  false}},
        {"fs.stat",           {handlers::FsStat,           Tier::Observe,  false}},
        {"fs.mounts",         {handlers::FsMounts,         Tier::Observe,  false}},
        {"fs.hash",           {handlers::FsHash,           Tier::Observe,  false}},
        {"fs.freespace",      {handlers::FsFreeSpace,      Tier::Observe,  false}},
        {"fs.find",           {handlers::FsFind,           Tier::Observe,  false}},
        {"fs.grep",           {handlers::FsGrep,           Tier::Observe,  false}},
        {"save.list",         {handlers::SaveList,         Tier::Observe,  false}},
        {"save.mount",        {handlers::SaveMount,        Tier::Observe,  false}},
        {"save.unmount",      {handlers::SaveUnmount,      Tier::Observe,  false}},
        {"nand.read",         {handlers::NandRead,         Tier::Observe,  false}},
        {"album.list",        {handlers::AlbumList,        Tier::Observe,  false}},
        {"album.download",    {handlers::AlbumDownload,    Tier::Observe,  false}},
        {"i2c.read",          {handlers::I2cRead,          Tier::Observe,  false}},
        {"gpio.read",         {handlers::GpioRead,         Tier::Observe,  false}},
        {"cheat.status",      {handlers::CheatStatus,      Tier::Observe,  false}},
        {"cheat.list",        {handlers::CheatList,        Tier::Observe,  false}},
        {"cheat.meta",        {handlers::CheatMeta,        Tier::Observe,  false}},
        {"cheat.mappings",    {handlers::CheatMappings,    Tier::Observe,  false}},
        {"cheat.read_mem",    {handlers::CheatReadMem,     Tier::Observe,  false}},
        {"search.begin",      {handlers::SearchBegin,      Tier::Observe,  false}},
        {"search.next",       {handlers::SearchNext,       Tier::Observe,  false}},
        {"search.results",    {handlers::SearchResults,    Tier::Observe,  false}},
        {"search.reset",      {handlers::SearchReset,      Tier::Observe,  false}},

        // --- control: things a person at the console could do --------------
        {"input",             {handlers::Input,            Tier::Control,  false}},
        {"touch",             {handlers::Touch,            Tier::Control,  false}},
        {"type_text",         {handlers::TypeText,         Tier::Control,  false}},
        {"launch",            {handlers::Launch,           Tier::Control,  true}},
        {"terminate",         {handlers::Terminate,        Tier::Control,  true}},
        {"reboot",            {handlers::Reboot,           Tier::Control,  true}},
        {"shutdown",          {handlers::Shutdown,         Tier::Control,  true}},
        {"agent.restart",     {handlers::AgentRestart,     Tier::Control,  true}},
        {"set_brightness",    {handlers::SetBrightness,    Tier::Control,  true}},
        {"set_auto_brightness", {handlers::SetAutoBrightness, Tier::Control, true}},
        {"set_volume",        {handlers::SetVolume,        Tier::Control,  true}},
        {"set_sleep",         {handlers::SetSleep,         Tier::Control,  true}},
        {"set_time",          {handlers::SetTime,          Tier::Control,  true}},
        {"set_wireless",      {handlers::SetWireless,      Tier::Control,  true}},
        {"fs.write",          {handlers::FsWrite,          Tier::Control,  true}},
        {"fs.delete",         {handlers::FsDelete,         Tier::Control,  true}},
        {"fs.mkdir",          {handlers::FsMkdir,          Tier::Control,  true}},
        {"fs.rename",         {handlers::FsRename,         Tier::Control,  true}},
        {"fs.copy",           {handlers::FsCopy,           Tier::Control,  true}},
        {"save.backup",       {handlers::BackupSave,       Tier::Control,  true}},
        // Attaching pauses the target and read_mem only observes, but both
        // require debug authority over another process, so they sit here.
        {"debug.attach",      {handlers::DebugAttach,      Tier::Control,  true}},
        {"debug.detach",      {handlers::DebugDetach,      Tier::Control,  true}},
        {"debug.continue",    {handlers::DebugContinue,    Tier::Control,  true}},
        {"debug.break",       {handlers::DebugBreak,       Tier::Control,  true}},
        {"debug.poll_events", {handlers::DebugPollEvents,  Tier::Control,  false}},
        {"debug.read_mem",    {handlers::DebugReadMem,     Tier::Control,  false}},
        {"debug.memmap",      {handlers::DebugMemMap,      Tier::Control,  false}},
        {"debug.threads",     {handlers::DebugThreads,     Tier::Control,  false}},
        {"debug.registers",   {handlers::DebugRegisters,   Tier::Control,  false}},
        {"debug.search",      {handlers::DebugSearch,      Tier::Control,  false}},
        {"debug.modules",     {handlers::DebugModules,     Tier::Observe,  false}},
        {"debug.backtrace",   {handlers::DebugBacktrace,   Tier::Control,  false}},
        {"cheat.toggle",      {handlers::CheatToggle,      Tier::Control,  true}},
        // Breakpoints only stop the target; they do not alter its state.
        {"debug.set_watchpoint",  {handlers::DebugSetWatchpoint,  Tier::Control, true}},
        {"debug.set_breakpoint",  {handlers::DebugSetBreakpoint,  Tier::Control, true}},
        {"debug.clear_breakpoint",{handlers::DebugClearBreakpoint,Tier::Control, true}},

        // --- invasive: can brick, damage, or irreversibly destroy ----------
        {"save.restore",      {handlers::RestoreSave,      Tier::Invasive, true}},
        {"uninstall",         {handlers::Uninstall,        Tier::Invasive, true}},
        {"reboot_to_payload", {handlers::RebootToPayload,  Tier::Invasive, true}},
        {"set_clocks",        {handlers::SetClocks,        Tier::Invasive, true}},
        {"debug.write_mem",   {handlers::DebugWriteMem,    Tier::Invasive, true}},
        {"debug.write_registers", {handlers::DebugWriteRegisters, Tier::Invasive, true}},
        {"debug.set_hwbp_raw",{handlers::DebugSetHwBpRaw,  Tier::Invasive, true}},
        {"cheat.write_mem",   {handlers::CheatWriteMem,    Tier::Invasive, true}},
        {"cheat.freeze",      {handlers::FreezeAddress,    Tier::Invasive, true}},
        {"cheat.unfreeze",    {handlers::UnfreezeAddress,  Tier::Invasive, true}},
        {"i2c.write",         {handlers::I2cWrite,         Tier::Invasive, true}},
    };
    return table;
}

// Per-operation socket timeout. Without this a half-open connection (client
// laptop sleeps, Wi-Fi drops mid-frame) parks the single service thread in
// recv() forever and the agent is unreachable until the console reboots.
constexpr int kIdleTimeoutSec = 30;   // between frames: client is just quiet
constexpr int kIoTimeoutSec = 15;     // mid-frame: bytes should already be flowing

// Apply a recv timeout to subsequent reads on this fd.
void SetRecvTimeout(int fd, int seconds) {
    struct timeval tv {};
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

// Why a timeout fired, so callers can tell "idle client" from "broken client".
enum class IoResult { Ok, Timeout, Closed };

// Read exactly n bytes. Distinguishes timeout from EOF/error so the caller can
// decide whether a quiet client is still worth keeping.
IoResult ReadExactEx(int fd, void* buf, size_t n) {
    uint8_t* p = (uint8_t*)buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(fd, p + got, n - got, 0);
        if (r == 0) return IoResult::Closed;  // orderly shutdown
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // A timeout part-way through a frame means the peer died
                // mid-write; we cannot resync the stream, so treat it as fatal.
                return got == 0 ? IoResult::Timeout : IoResult::Closed;
            }
            return IoResult::Closed;
        }
        got += (size_t)r;
    }
    return IoResult::Ok;
}

bool ReadExact(int fd, void* buf, size_t n) {
    return ReadExactEx(fd, buf, n) == IoResult::Ok;
}

bool WriteAll(int fd, const void* buf, size_t n) {
    const uint8_t* p = (const uint8_t*)buf;
    size_t sent = 0;
    while (sent < n) {
        ssize_t w = send(fd, p + sent, n - sent, 0);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        sent += (size_t)w;
    }
    return true;
}

// Timeouts + keepalive on an accepted connection. Keepalive is the backstop for
// the case recv timeouts cannot see: a peer that vanished without sending FIN
// while we are blocked in send().
void ConfigureConnSocket(int fd) {
    struct timeval tv {};
    tv.tv_sec = kIoTimeoutSec;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    SetRecvTimeout(fd, kIdleTimeoutSec);

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
#ifdef TCP_KEEPIDLE
    // 10 s rather than 30: this console's Wi-Fi drops into power-save when the
    // link goes idle, which showed up as 30%+ packet loss and multi-second
    // connect times. Frequent keepalives keep the association warm.
    int idle = 10, intvl = 5, cnt = 4;   // dead peer detected in ~30 s
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
#endif
    // Replies are small and latency matters more than packing.
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
}

// One frame: u32 json_len + json [+ u64 bin_len + bytes].
//
// Returns Timeout only when nothing at all arrived (an idle but healthy
// client); once the length prefix is in we switch to the shorter mid-frame
// timeout, and any stall after that is reported as Closed because a partially
// read frame leaves the stream unresyncable.
IoResult ReadFrame(int fd, json::Value& msg, std::vector<uint8_t>& payload) {
    uint32_t jlen = 0;
    IoResult r = ReadExactEx(fd, &jlen, 4);
    if (r != IoResult::Ok) return r;

    // Frame started: the rest should follow promptly.
    SetRecvTimeout(fd, kIoTimeoutSec);
    struct RestoreIdle {
        int fd;
        ~RestoreIdle() { SetRecvTimeout(fd, kIdleTimeoutSec); }
    } restore{fd};

    if (jlen == 0 || jlen > kMaxJson) {
        LOG_WARN("frame json length %u out of range", jlen);
        return IoResult::Closed;
    }
    std::string json_text;
    json_text.resize(jlen);
    if (!ReadExact(fd, &json_text[0], jlen)) return IoResult::Closed;
    if (!json::Value::parse(json_text, msg)) {
        LOG_WARN("frame JSON parse failed");
        return IoResult::Closed;
    }
    payload.clear();
    if (msg.has("bin")) {
        int64_t blen = msg["bin"].as_int(-1);
        if (blen < 0 || (uint64_t)blen > kMaxBinary) {
            LOG_WARN("frame bin length %lld out of range", (long long)blen);
            return IoResult::Closed;
        }
        // Wire binary length is a u64 prefix even though `bin` echoes it.
        uint64_t wire_len = 0;
        if (!ReadExact(fd, &wire_len, 8)) return IoResult::Closed;
        if (wire_len != (uint64_t)blen || wire_len > kMaxBinary) return IoResult::Closed;
        payload.resize(wire_len);
        if (wire_len && !ReadExact(fd, payload.data(), wire_len)) return IoResult::Closed;
    }
    return IoResult::Ok;
}

bool WriteFrame(int fd, const json::Value& msg, const std::vector<uint8_t>& payload) {
    std::string text = msg.dump();
    uint32_t jlen = (uint32_t)text.size();
    if (!WriteAll(fd, &jlen, 4)) return false;
    if (!WriteAll(fd, text.data(), text.size())) return false;
    if (!payload.empty()) {
        uint64_t blen = payload.size();
        if (!WriteAll(fd, &blen, 8)) return false;
        if (!WriteAll(fd, payload.data(), payload.size())) return false;
    }
    return true;
}

const char* TierName(Tier t) {
    switch (t) {
        case Tier::Observe: return "observe";
        case Tier::Control: return "control";
        case Tier::Invasive: return "invasive";
    }
    return "observe";
}

// Enforce the configured capability tier, plus the per-feature sub-gates that
// sit inside the invasive tier. Refusals say exactly which config change would
// permit the command, so an operator is not left guessing — and say it without
// performing any part of the action first.
bool CheckTier(const std::string& cmd, const Command& c, const AgentConfig& cfg,
               Reply& reply) {
    if ((int)c.tier > (int)cfg.tier) {
        LOG_WARN("refused '%s': needs tier=%s, configured tier=%s", cmd.c_str(),
                 TierName(c.tier), TierName(cfg.tier));
        return Fail(reply, "tier_denied",
                    std::string("'") + cmd + "' requires tier=" + TierName(c.tier) +
                        " but the agent is configured for tier=" + TierName(cfg.tier) +
                        ". Raise `tier` in sd:/config/switch-agentd/config.ini and "
                        "restart the agent.");
    }

    // Sub-gates. These are deliberately independent of the tier: reaching the
    // invasive tier is necessary but not sufficient for the sharpest edges.
    if ((cmd == "i2c.write" || cmd == "gpio.read" || cmd == "i2c.read") &&
        !cfg.allow_hardware)
        return Fail(reply, "disabled",
                    "raw hardware bus access requires allow_hardware=true in config");

    if (cmd == "set_clocks" && !cfg.allow_overclock)
        return Fail(reply, "disabled",
                    "clock control requires allow_overclock=true in config");

    if ((cmd == "uninstall" || cmd == "save.restore") && !cfg.allow_nand_write)
        return Fail(reply, "disabled",
                    std::string("'") + cmd +
                        "' writes to NAND and requires allow_nand_write=true in config");

    // sysMMC has no undo. emuMMC lives on the SD card, so the same mistake is
    // recoverable by restoring an image — which is why it is not blocked here.
    if (cmd == "i2c.write" && !cfg.is_emummc)
        LOG_WARN("i2c write on sysMMC: this can permanently damage hardware");

    return true;
}

// Constant-time string compare so token checks don't leak length/content.
bool SecureEqual(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < a.size(); i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

}  // namespace

// Set when a save is mounted read-only; see handlers/savedata.cpp.
bool g_save_mounted = false;

bool ResolveDevicePath(const std::string& device, const std::string& in,
                       std::string& out, bool* writable) {
    std::string norm;
    if (!NormalizePath(in, norm)) return false;

    if (device.empty() || device == "sd" || device == "sdmc") {
        out = "sdmc:" + norm;
        if (writable) *writable = true;
        return true;
    }
    if (device == "save") {
        if (!g_save_mounted) return false;   // nothing mounted to address
        out = std::string(kSaveMountName) + ":" + norm;
        if (writable) *writable = false;     // mounted read-only, always
        return true;
    }
    return false;
}

bool IsDryRun(const Request& req) {
    const json::Value& v = req["dry_run"];
    return v.as_bool(false) || v.as_int(0) == 1;
}

bool Fail(Reply& reply, const char* code, const char* message) {
    reply.json = json::Value::object();
    json::Value err = json::Value::object();
    err.set("code", code);
    err.set("message", message);
    reply.json.set("error", std::move(err));
    reply.out.clear();
    return false;
}

bool Fail(Reply& reply, const char* code, const std::string& message) {
    return Fail(reply, code, message.c_str());
}

// Challenge-response authentication.
//
// The legacy handshake sent the shared token in cleartext on every connect, so
// anyone passively watching the LAN learned a credential granting memory
// patching, raw i2c and reboot — and could replay it forever. Here the SERVER
// issues a fresh random nonce and the client proves knowledge of the token by
// returning HMAC-SHA256(token, nonce). The token never crosses the wire, and a
// captured exchange is useless because that nonce is never offered again.
//
// The client picking the nonce would NOT achieve this: an attacker could replay
// a captured (nonce, hmac) pair verbatim. It has to come from this side.
//
// Legacy plaintext auth still works unless require_hmac_auth is set, so older
// clients keep running; setting the flag closes the door.
bool Server::Authenticate(int fd) {
    json::Value msg;
    std::vector<uint8_t> payload;
    // An unauthenticated peer gets the short timeout: it must say hello
    // promptly or lose the (single) connection slot.
    SetRecvTimeout(fd, kIoTimeoutSec);
    IoResult hello_r = ReadFrame(fd, msg, payload);
    if (hello_r != IoResult::Ok) {
        SetRecvTimeout(fd, kIdleTimeoutSec);
        if (hello_r == IoResult::Timeout) LOG_WARN("peer connected but never sent hello");
        return false;
    }
    if (msg["cmd"].as_string() != "hello") {
        SetRecvTimeout(fd, kIdleTimeoutSec);
        LOG_WARN("first frame was not hello");
        return false;
    }

    int64_t hello_id = msg["id"].as_int();
    bool authed = false;

    if (msg["auth"].as_string() == "hmac") {
        // Round two: hand out a nonce and require the MAC over it.
        uint8_t nonce[32];
        randomGet(nonce, sizeof(nonce));
        char nonce_hex[sizeof(nonce) * 2 + 1];
        for (size_t i = 0; i < sizeof(nonce); i++)
            std::snprintf(nonce_hex + i * 2, 3, "%02x", nonce[i]);

        json::Value ch = json::Value::object();
        ch.set("id", hello_id);
        ch.set("challenge", nonce_hex);
        ch.set("algo", "hmac-sha256");
        if (!WriteFrame(fd, ch, {})) {
            SetRecvTimeout(fd, kIdleTimeoutSec);
            return false;
        }

        json::Value resp;
        std::vector<uint8_t> rp;
        if (ReadFrame(fd, resp, rp) != IoResult::Ok) {
            SetRecvTimeout(fd, kIdleTimeoutSec);
            LOG_WARN("client did not answer the auth challenge");
            return false;
        }
        std::string presented = resp["hmac"].as_string();
        if (!cfg_.token.empty() && presented.size() == SHA256_HASH_SIZE * 2) {
            uint8_t mac[SHA256_HASH_SIZE];
            hmacSha256CalculateMac(mac, cfg_.token.data(), cfg_.token.size(),
                                   nonce_hex, std::strlen(nonce_hex));
            char hex[SHA256_HASH_SIZE * 2 + 1];
            for (size_t i = 0; i < SHA256_HASH_SIZE; i++)
                std::snprintf(hex + i * 2, 3, "%02x", mac[i]);
            authed = SecureEqual(presented, hex);
        }
        if (!authed) LOG_WARN("hmac auth rejected");
        msg = std::move(resp);              // reply to the response frame's id
        hello_id = msg["id"].as_int(hello_id);
    } else if (!cfg_.require_hmac_auth) {
        authed = !cfg_.token.empty() &&
                 SecureEqual(msg["token"].as_string(), cfg_.token);
        if (authed) LOG_DEBUG("legacy plaintext auth accepted");
    } else {
        LOG_WARN("plaintext auth refused: require_hmac_auth is set");
    }

    SetRecvTimeout(fd, kIdleTimeoutSec);

    if (!authed) {
        // Backoff on the failure path only. Without it the token is
        // brute-forceable at line rate; with it an attacker gets a handful of
        // attempts a minute, and a legitimate client never waits because it
        // does not fail.
        svcSleepThread(2'000'000'000ULL);
        LOG_WARN("auth rejected");
        json::Value r = json::Value::object();
        r.set("id", hello_id);
        json::Value err = json::Value::object();
        err.set("code", "auth");
        err.set("message", "invalid credentials");
        r.set("error", std::move(err));
        WriteFrame(fd, r, {});
        return false;
    }

    SetSysFirmwareVersion fw{};
    setsysGetFirmwareVersion(&fw);
    json::Value r = json::Value::object();
    r.set("id", msg["id"].as_int());
    r.set("ok", true);
    r.set("agent_version", "0.2.0");
    r.set("protocol_version", 1);
    char fwbuf[32];
    std::snprintf(fwbuf, sizeof(fwbuf), "%u.%u.%u", fw.major, fw.minor, fw.micro);
    r.set("fw", fwbuf);

    // Capability negotiation: tell the client what this build can actually do,
    // so it can hide tools that would only fail at call time instead of
    // discovering the limits one refusal at a time.
    r.set("tier", TierName(cfg_.tier));
    r.set("emummc", cfg_.is_emummc);
    r.set("allow_nand_write", cfg_.allow_nand_write);
    r.set("allow_overclock", cfg_.allow_overclock);
    r.set("allow_hardware", cfg_.allow_hardware);

    json::Value cmds = json::Value::array();
    for (const auto& kv : Dispatch()) {
        // Only advertise what the current tier would actually permit.
        if ((int)kv.second.tier <= (int)cfg_.tier) cmds.push(json::Value(kv.first));
    }
    r.set("commands", std::move(cmds));
    if (!WriteFrame(fd, r, {})) return false;
    handlers::WatchdogOnConnect();   // reachable: the boot streak is broken
    LOG_INFO("client authenticated");
    return true;
}

void Server::ServeConnection(int fd) {
    if (!Authenticate(fd)) return;

    const auto& dispatch = Dispatch();
    // An interactive agent can legitimately sit idle between commands, so a
    // single idle timeout is not grounds for hanging up. Reap only after a
    // sustained silence; TCP keepalive covers peers that vanish outright.
    constexpr int kMaxIdleTimeouts = 10;  // 10 × 30 s ≈ 5 min
    int idle_timeouts = 0;

    while (true) {
        json::Value msg;
        std::vector<uint8_t> payload;
        IoResult r = ReadFrame(fd, msg, payload);
        if (r == IoResult::Timeout) {
            if (++idle_timeouts >= kMaxIdleTimeouts) {
                LOG_INFO("client idle for %d s; closing connection",
                         kMaxIdleTimeouts * kIdleTimeoutSec);
                break;
            }
            continue;
        }
        if (r != IoResult::Ok) break;
        idle_timeouts = 0;

        int64_t id = msg["id"].as_int();
        std::string cmd = msg["cmd"].as_string();

        Request req{msg, payload, cfg_};
        Reply reply;
        auto it = dispatch.find(cmd);
        if (it == dispatch.end()) {
            Fail(reply, "unknown_command", cmd);
        } else if (!CheckTier(cmd, it->second, cfg_, reply)) {
            // CheckTier populated the refusal.
        } else if (it->second.mutating && IsDryRun(req)) {
            // Preview only: describe the effect, change nothing. Handlers are
            // never entered, so there is no way for one to act by accident.
            reply.json.set("dry_run", true);
            reply.json.set("would_run", cmd);
            reply.json.set("tier", TierName(it->second.tier));
            reply.json.set("note", "not executed; re-send without dry_run to apply");
            LOG_INFO("dry-run: %s", cmd.c_str());
        } else {
            bool ok = it->second.fn(req, reply);
            // Journal anything that changed the console. Written after the fact
            // with the outcome, so the trail distinguishes "was attempted" from
            // "succeeded" — the difference that matters when reconstructing how
            // a console got into a bad state.
            if (it->second.mutating)
                handlers::JournalAppend(cmd.c_str(), ok ? "ok" : "FAILED");
            LOG_DEBUG("cmd %s -> %s (%zu bytes bin)", cmd.c_str(),
                      reply.json.has("error") ? "error" : "ok", reply.out.size());
        }

        reply.json.set("id", id);
        if (!reply.out.empty()) reply.json.set("bin", (int64_t)reply.out.size());
        if (!WriteFrame(fd, reply.json, reply.out)) break;
    }

    // Client gone: hand control back to the physical controller.
    vpad::Release();
    LOG_INFO("client disconnected");
}

namespace {
// Create, bind, and listen on the port. Returns -1 on failure.
int OpenListener(uint16_t port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    // Backlog of 4 rather than 1. The agent still serves one connection at a
    // time, but with a backlog of 1 a second client was actively REFUSED rather
    // than queued — which is how a long-running command (a memory scan, a
    // wait_event) turned "wait your turn" into "connection refused" for every
    // other session. Queuing costs nothing and makes the failure mode patience
    // instead of an error.
    if (bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0 || listen(fd, 4) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}
}  // namespace

void Server::Run() {
    // Outer loop owns the listening socket. Sleep/wake can invalidate the
    // socket (the network stack is torn down on sleep), after which accept()
    // fails perpetually. When that happens we tear the listener down and
    // rebuild it, so the agent self-heals on wake without needing a reboot.
    while (true) {
        int listen_fd = OpenListener(cfg_.port);
        if (listen_fd < 0) {
            LOG_ERROR("OpenListener(%u) failed: %d; retrying", cfg_.port, errno);
            svcSleepThread(1'000'000'000ULL);  // 1 s, then retry
            continue;
        }
        LOG_INFO("listening on port %u", cfg_.port);

        int consecutive_errors = 0;
        while (true) {
            sockaddr_in peer{};
            socklen_t plen = sizeof(peer);
            int fd = accept(listen_fd, (sockaddr*)&peer, &plen);
            if (fd < 0) {
                // Preferred path: psc told us the system slept, so rebuild the
                // listener as soon as we are awake rather than waiting for a
                // run of errors to accumulate.
                if (power_state::TakeWokeFlag()) {
                    LOG_INFO("rebuilding listener after wake");
                    break;
                }
                if (power_state::IsAsleep()) {
                    svcSleepThread(200'000'000ULL);  // asleep: don't spin
                    continue;
                }
                // Fallback for builds where psc:m was unavailable: a sustained
                // run of failures means the socket is dead — rebuild it.
                if (++consecutive_errors >= 20) {
                    LOG_WARN("listener wedged (accept errno %d); rebuilding", errno);
                    break;
                }
                svcSleepThread(100'000'000ULL);  // 100 ms
                continue;
            }
            consecutive_errors = 0;
            LOG_INFO("client connected from %s", inet_ntoa(peer.sin_addr));
            ConfigureConnSocket(fd);
            ServeConnection(fd);
            close(fd);
        }
        close(listen_fd);
    }
}

}  // namespace agent
