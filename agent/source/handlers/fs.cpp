// Filesystem handlers: list/read/write/delete/mkdir/rename/stat + save backup.
// SD card is mounted at sdmc:/ (see main.cpp). Protocol paths use '/' as the
// SD root; we map that onto sdmc:/ and refuse traversal outside mounted devices.
#include "../protocol.hpp"

#include <switch.h>

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <vector>

#include "../log.hpp"

namespace agent {
namespace handlers {

namespace {

// Map a normalized protocol path to a device path. Only sdmc: for now; NAND is
// intentionally not writable here (bricking risk) and lives behind separate
// dedicated handlers if ever added.
bool ResolveSd(const std::string& in, std::string& dev) {
    return ResolveSdPath(in, dev);
}

const char* EntryType(unsigned char d_type, const std::string& full) {
    if (d_type == DT_DIR) return "dir";
    if (d_type == DT_REG) return "file";
    struct stat st;
    if (stat(full.c_str(), &st) == 0) return S_ISDIR(st.st_mode) ? "dir" : "file";
    return "file";
}

}  // namespace

bool FsList(const Request& req, Reply& reply) {
    std::string dev;
    if (!ResolveDevicePath(req["device"].as_string("sd"), req["path"].as_string("/"),
                           dev, nullptr))
        return Fail(reply, "bad_path", "invalid path, or unknown/unmounted device");
    DIR* dir = opendir(dev.c_str());
    if (!dir) return Fail(reply, "not_found", "cannot open directory");

    json::Value entries = json::Value::array();
    struct dirent* de;
    while ((de = readdir(dir)) != nullptr) {
        std::string name = de->d_name;
        if (name == "." || name == "..") continue;
        std::string full = dev + "/" + name;
        json::Value e = json::Value::object();
        e.set("name", name);
        e.set("type", EntryType(de->d_type, full));
        struct stat st;
        if (stat(full.c_str(), &st) == 0) {
            e.set("size", (int64_t)st.st_size);
            e.set("mtime", (int64_t)st.st_mtime);
        }
        entries.push(std::move(e));
    }
    closedir(dir);
    reply.json.set("entries", std::move(entries));
    return true;
}

bool FsRead(const Request& req, Reply& reply) {
    std::string dev;
    if (!ResolveDevicePath(req["device"].as_string("sd"), req["path"].as_string(),
                           dev, nullptr))
        return Fail(reply, "bad_path", "invalid path, or unknown/unmounted device");
    int64_t offset = req["offset"].as_int(0);
    int64_t len = req["len"].as_int(1 << 20);
    if (len < 0 || len > (1 << 20)) len = 1 << 20;

    FILE* f = std::fopen(dev.c_str(), "rb");
    if (!f) return Fail(reply, "not_found", "cannot open file");
    std::fseek(f, offset, SEEK_SET);
    reply.out.resize(len);
    size_t got = std::fread(reply.out.data(), 1, len, f);
    bool eof = std::feof(f) != 0;
    std::fclose(f);
    reply.out.resize(got);
    reply.json.set("eof", eof || got < (size_t)len);
    return true;
}


namespace {
// Mutating handlers are SD-only. Accepting the parameter and then ignoring it
// is the dangerous case: a caller who passes device="save" believes they are
// writing to the save and is actually writing to the SD card. Refuse loudly.
bool RequireWritableSd(const Request& req, Reply& reply, std::string& dev) {
    std::string device = req["device"].as_string("sd");
    if (device != "sd" && device != "sdmc") {
        Fail(reply, "read_only_device",
             "device '" + device + "' is mounted read-only; only the SD card "
             "(device=\"sd\") can be written to");
        return false;
    }
    if (!ResolveSdPath(req["path"].as_string(), dev)) {
        Fail(reply, "bad_path", "invalid or unsafe path");
        return false;
    }
    return true;
}
}  // namespace

bool FsWrite(const Request& req, Reply& reply) {
    std::string dev;
    if (!RequireWritableSd(req, reply, dev)) return false;
    int64_t offset = req["offset"].as_int(0);

    // offset 0 truncates/creates; subsequent offsets append into the file.
    FILE* f = std::fopen(dev.c_str(), offset == 0 ? "wb" : "r+b");
    if (!f && offset != 0) f = std::fopen(dev.c_str(), "wb");
    if (!f) return Fail(reply, "io_error", "cannot open file for writing");
    std::fseek(f, offset, SEEK_SET);
    size_t w = std::fwrite(req.payload.data(), 1, req.payload.size(), f);
    std::fflush(f);
    std::fclose(f);
    reply.json.set("written", (int64_t)w);
    return true;
}

bool FsDelete(const Request& req, Reply& reply) {
    std::string dev;
    if (!RequireWritableSd(req, reply, dev)) return false;
    struct stat st;
    if (stat(dev.c_str(), &st) != 0) return Fail(reply, "not_found", "no such path");
    int rc = S_ISDIR(st.st_mode) ? rmdir(dev.c_str()) : remove(dev.c_str());
    if (rc != 0) return Fail(reply, "io_error", "delete failed");
    reply.json.set("ok", true);
    return true;
}

bool FsMkdir(const Request& req, Reply& reply) {
    std::string dev;
    if (!RequireWritableSd(req, reply, dev)) return false;
    if (mkdir(dev.c_str(), 0777) != 0) return Fail(reply, "io_error", "mkdir failed");
    reply.json.set("ok", true);
    return true;
}

bool FsRename(const Request& req, Reply& reply) {
    std::string device = req["device"].as_string("sd");
    if (device != "sd" && device != "sdmc")
        return Fail(reply, "read_only_device",
                    "rename is only possible on the SD card (device=\"sd\")");
    std::string from, to;
    if (!ResolveSdPath(req["from"].as_string(), from) ||
        !ResolveSdPath(req["to"].as_string(), to))
        return Fail(reply, "bad_path", "invalid or unsafe path");
    if (rename(from.c_str(), to.c_str()) != 0)
        return Fail(reply, "io_error", "rename failed");
    reply.json.set("ok", true);
    return true;
}

bool FsStat(const Request& req, Reply& reply) {
    std::string dev;
    if (!ResolveDevicePath(req["device"].as_string("sd"), req["path"].as_string(),
                           dev, nullptr))
        return Fail(reply, "bad_path", "invalid path, or unknown/unmounted device");
    struct stat st;
    if (stat(dev.c_str(), &st) != 0) return Fail(reply, "not_found", "no such path");
    reply.json.set("type", S_ISDIR(st.st_mode) ? "dir" : "file");
    reply.json.set("size", (int64_t)st.st_size);
    reply.json.set("mtime", (int64_t)st.st_mtime);
    return true;
}

// List the logical mount points the agent exposes.
bool FsMounts(const Request& req, Reply& reply) {
    json::Value mounts = json::Value::array();
    json::Value sd = json::Value::object();
    sd.set("name", "/");
    sd.set("device", "sdmc");
    sd.set("writable", true);
    mounts.push(std::move(sd));
    if (req.cfg.allow_nand_write || true) {
        // NAND user partition is exposed read-only via the nand.read command
        // (gated). Advertise it so clients know it exists.
        json::Value nand = json::Value::object();
        nand.set("name", "nand:user");
        nand.set("device", "bis:UserDataRoot");
        nand.set("writable", false);
        nand.set("access", "nand.read only");
        mounts.push(std::move(nand));
    }
    reply.json.set("mounts", std::move(mounts));
    return true;
}

// Read a file from a NAND BIS partition, read-only. Reading NAND can expose
// sensitive data (keys/saves) so it's a distinct command; it can never write.
bool NandRead(const Request& req, Reply& reply) {
    std::string path = req["path"].as_string();  // e.g. "/save/..." within partition
    if (path.empty() || path[0] != '/' || path.find("..") != std::string::npos)
        return Fail(reply, "bad_path", "invalid path");
    // Default to the user data partition; callers can request others by id.
    FsBisPartitionId part = (FsBisPartitionId)req["partition"].as_int(FsBisPartitionId_UserDataRoot);

    FsFileSystem fs;
    Result rc = fsOpenBisFileSystem(&fs, part, "");
    if (R_FAILED(rc)) return Fail(reply, "bis_failed", "cannot open BIS partition");

    FsFile file;
    rc = fsFsOpenFile(&fs, path.c_str(), FsOpenMode_Read, &file);
    if (R_FAILED(rc)) {
        fsFsClose(&fs);
        return Fail(reply, "not_found", "cannot open NAND file");
    }
    int64_t offset = req["offset"].as_int(0);
    int64_t len = req["len"].as_int(1 << 20);
    if (len < 0 || len > (1 << 20)) len = 1 << 20;
    reply.out.resize(len);
    u64 read = 0;
    rc = fsFileRead(&file, offset, reply.out.data(), len, FsReadOption_None, &read);
    fsFileClose(&file);
    fsFsClose(&fs);
    if (R_FAILED(rc)) return Fail(reply, "io_error", "NAND read failed");
    reply.out.resize(read);
    reply.json.set("eof", read < (u64)len);
    return true;
}

namespace {
// Recursive directory copy for save backup/restore.
// One buffer reused by every level of the copy recursion. Allocated on first
// use rather than per frame, so depth costs nothing.
std::vector<char>& CopyBuffer() {
    static std::vector<char> buf(64 * 1024);
    return buf;
}

bool CopyTree(const std::string& src, const std::string& dst) {
    mkdir(dst.c_str(), 0777);
    DIR* dir = opendir(src.c_str());
    if (!dir) return false;
    bool ok = true;
    struct dirent* de;
    while ((de = readdir(dir)) != nullptr) {
        std::string name = de->d_name;
        if (name == "." || name == "..") continue;
        std::string s = src + "/" + name;
        std::string d = dst + "/" + name;
        struct stat st;
        if (stat(s.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            ok &= CopyTree(s, d);
        } else {
            FILE* in = std::fopen(s.c_str(), "rb");
            FILE* out = std::fopen(d.c_str(), "wb");
            if (in && out) {
                // Heap, and shared across recursion levels: a 64 KiB local
                // array blows the 32 KiB main-thread stack outright, and this
                // function recurses per directory.
                std::vector<char>& buf = CopyBuffer();
                size_t n;
                while ((n = std::fread(buf.data(), 1, buf.size(), in)) > 0)
                    std::fwrite(buf.data(), 1, n, out);
            } else {
                ok = false;
            }
            if (in) std::fclose(in);
            if (out) std::fclose(out);
        }
    }
    closedir(dir);
    return ok;
}
}  // namespace

// Mount a title's save data and copy it to/from an SD backup directory.
// tid: hex title id; the current primary user is used unless `uid_lo`/`uid_hi`
// are given.
bool BackupSave(const Request& req, Reply& reply) {
    uint64_t tid = std::strtoull(req["tid"].as_string("0").c_str(), nullptr, 16);
    if (!tid) return Fail(reply, "bad_arg", "missing tid");
    AccountUid uid{{(u64)req["uid_lo"].as_int(0), (u64)req["uid_hi"].as_int(0)}};

    Result rc = fsdevMountSaveData("save", tid, uid);
    if (R_FAILED(rc)) return Fail(reply, "mount_failed", "cannot mount save data");
    std::string dst = "sdmc:/switch-agentd/saves/" +
                      std::string(req["tid"].as_string());
    mkdir("sdmc:/switch-agentd", 0777);
    mkdir("sdmc:/switch-agentd/saves", 0777);
    bool ok = CopyTree("save:/", dst);
    fsdevUnmountDevice("save");
    if (!ok) return Fail(reply, "io_error", "backup copy failed");
    reply.json.set("ok", true);
    reply.json.set("path", dst);
    return true;
}

bool RestoreSave(const Request& req, Reply& reply) {
    uint64_t tid = std::strtoull(req["tid"].as_string("0").c_str(), nullptr, 16);
    if (!tid) return Fail(reply, "bad_arg", "missing tid");
    AccountUid uid{{(u64)req["uid_lo"].as_int(0), (u64)req["uid_hi"].as_int(0)}};
    std::string src = "sdmc:/switch-agentd/saves/" +
                      std::string(req["tid"].as_string());

    Result rc = fsdevMountSaveData("save", tid, uid);
    if (R_FAILED(rc)) return Fail(reply, "mount_failed", "cannot mount save data");
    bool ok = CopyTree(src, "save:/");
    if (ok) rc = fsdevCommitDevice("save");  // persist writes back to NAND
    fsdevUnmountDevice("save");
    if (!ok || R_FAILED(rc)) return Fail(reply, "io_error", "restore/commit failed");
    reply.json.set("ok", true);
    return true;
}

// SHA-256 of a file on the SD card, streamed so file size is not bounded by
// heap. Used to verify uploads landed intact before acting on them — notably
// agent self-update, where a truncated write means the agent does not come back
// and recovery needs physical access to the SD card.
bool FsHash(const Request& req, Reply& reply) {
    std::string dev;
    if (!ResolveDevicePath(req["device"].as_string("sd"), req["path"].as_string(),
                           dev, nullptr))
        return Fail(reply, "bad_path", "invalid path, or unknown/unmounted device");

    std::string algo = req["algo"].as_string("sha256");
    if (algo != "sha256") return Fail(reply, "bad_arg", "only sha256 is supported");

    FILE* f = std::fopen(dev.c_str(), "rb");
    if (!f) return Fail(reply, "not_found", "cannot open file");

    Sha256Context ctx;
    sha256ContextCreate(&ctx);
    std::vector<uint8_t> buf(64 * 1024);
    uint64_t total = 0;
    while (true) {
        size_t n = std::fread(buf.data(), 1, buf.size(), f);
        if (n == 0) break;
        sha256ContextUpdate(&ctx, buf.data(), n);
        total += n;
    }
    bool read_error = std::ferror(f) != 0;
    std::fclose(f);
    if (read_error) return Fail(reply, "io_error", "read failed while hashing");

    uint8_t digest[SHA256_HASH_SIZE];
    sha256ContextGetHash(&ctx, digest);

    char hex[SHA256_HASH_SIZE * 2 + 1];
    for (size_t i = 0; i < SHA256_HASH_SIZE; i++)
        std::snprintf(hex + i * 2, 3, "%02x", digest[i]);

    reply.json.set("ok", true);
    reply.json.set("sha256", hex);
    reply.json.set("size", (int64_t)total);
    return true;
}


// Free/total space for a device. Cheap, and the answer everyone wants before
// starting a transfer or a dump.
bool FsFreeSpace(const Request& req, Reply& reply) {
    std::string dev;
    bool writable = false;
    if (!ResolveDevicePath(req["device"].as_string("sd"), "/", dev, &writable))
        return Fail(reply, "bad_device", "unknown or unmounted device");

    // statvfs via the devoptab understands the fsdev mount names.
    struct statvfs st;
    if (statvfs(dev.c_str(), &st) != 0)
        return Fail(reply, "io_error", "cannot stat the filesystem");

    uint64_t bsize = st.f_frsize ? st.f_frsize : st.f_bsize;
    uint64_t total = (uint64_t)st.f_blocks * bsize;
    uint64_t avail = (uint64_t)st.f_bavail * bsize;
    reply.json.set("device", req["device"].as_string("sd"));
    reply.json.set("writable", writable);
    reply.json.set("total_bytes", (int64_t)total);
    reply.json.set("free_bytes", (int64_t)avail);
    reply.json.set("used_bytes", (int64_t)(total - avail));
    return true;
}

namespace {

// Shared recursion guard. Deep or looping trees would otherwise let one request
// run effectively forever while holding the single service thread.
constexpr int kMaxWalkDepth = 12;

struct FindCtx {
    std::string needle;      // lowercase substring to match, empty = match all
    bool files_only = false;
    int limit = 200;
    int scanned = 0;
    int max_scan = 40000;    // hard ceiling so a huge tree still terminates
    int found = 0;           // json::Value has no size(), so count as we go
    json::Value* out = nullptr;
};

std::string Lower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c += 32;
    return s;
}

void WalkFind(const std::string& dev_dir, const std::string& logical, FindCtx& ctx,
              int depth) {
    if (depth > kMaxWalkDepth) return;
    if (ctx.found >= ctx.limit || ctx.scanned >= ctx.max_scan) return;

    DIR* dir = opendir(dev_dir.c_str());
    if (!dir) return;
    struct dirent* de;
    while ((de = readdir(dir)) != nullptr) {
        if (ctx.found >= ctx.limit || ctx.scanned >= ctx.max_scan) break;
        std::string name = de->d_name;
        if (name == "." || name == "..") continue;
        ctx.scanned++;

        std::string child_dev = dev_dir + "/" + name;
        std::string child_log = logical == "/" ? "/" + name : logical + "/" + name;
        bool is_dir = de->d_type == DT_DIR;
        if (de->d_type == DT_UNKNOWN) {
            struct stat st;
            is_dir = (stat(child_dev.c_str(), &st) == 0) && S_ISDIR(st.st_mode);
        }

        bool matches = ctx.needle.empty() ||
                       Lower(name).find(ctx.needle) != std::string::npos;
        if (matches && !(ctx.files_only && is_dir)) {
            json::Value e = json::Value::object();
            e.set("path", child_log);
            e.set("type", is_dir ? "dir" : "file");
            struct stat st;
            if (!is_dir && stat(child_dev.c_str(), &st) == 0)
                e.set("size", (int64_t)st.st_size);
            ctx.out->push(std::move(e));
            ctx.found++;
        }
        if (is_dir) WalkFind(child_dev, child_log, ctx, depth + 1);
    }
    closedir(dir);
}

}  // namespace

// Recursive filename search, executed on the console.
//
// The alternative is listing every directory over the wire and filtering on the
// client, which for an SD card full of homebrew means thousands of round-trips.
bool FsFind(const Request& req, Reply& reply) {
    std::string dev;
    if (!ResolveDevicePath(req["device"].as_string("sd"), req["path"].as_string("/"),
                           dev, nullptr))
        return Fail(reply, "bad_path", "invalid path or unknown device");

    json::Value results = json::Value::array();
    FindCtx ctx;
    ctx.needle = Lower(req["name_contains"].as_string());
    ctx.files_only = req["files_only"].as_bool(false);
    ctx.limit = (int)req["limit"].as_int(200);
    if (ctx.limit < 1 || ctx.limit > 2000) ctx.limit = 200;
    ctx.out = &results;

    WalkFind(dev, req["path"].as_string("/"), ctx, 0);

    reply.json.set("scanned", (int64_t)ctx.scanned);
    reply.json.set("count", (int64_t)ctx.found);
    // Say so rather than quietly returning a partial answer.
    if (ctx.found >= ctx.limit) reply.json.set("truncated_by_limit", true);
    if (ctx.scanned >= ctx.max_scan) reply.json.set("truncated_by_scan_cap", true);
    reply.json.set("matches", std::move(results));
    return true;
}

// Substring search inside a single file, executed on the console. Returns
// matching lines with their offsets rather than shipping the whole file over.
bool FsGrep(const Request& req, Reply& reply) {
    std::string dev;
    if (!ResolveDevicePath(req["device"].as_string("sd"), req["path"].as_string(),
                           dev, nullptr))
        return Fail(reply, "bad_path", "invalid path or unknown device");
    std::string pattern = req["pattern"].as_string();
    if (pattern.empty()) return Fail(reply, "bad_arg", "need a 'pattern'");
    bool ignore_case = req["ignore_case"].as_bool(false);
    int limit = (int)req["limit"].as_int(100);
    if (limit < 1 || limit > 1000) limit = 100;

    FILE* f = std::fopen(dev.c_str(), "rb");
    if (!f) return Fail(reply, "not_found", "cannot open file");

    std::string needle = ignore_case ? Lower(pattern) : pattern;
    json::Value matches = json::Value::array();
    int found = 0;
    std::string line;
    int lineno = 0;
    int64_t offset = 0, line_start = 0;
    int ch;
    bool truncated = false;
    while ((ch = std::fgetc(f)) != EOF) {
        offset++;
        if (ch == '\n') {
            lineno++;
            std::string hay = ignore_case ? Lower(line) : line;
            if (hay.find(needle) != std::string::npos) {
                if (found >= limit) { truncated = true; break; }
                json::Value m = json::Value::object();
                m.set("line", (int64_t)lineno);
                m.set("offset", (int64_t)line_start);
                // Cap the echoed line so one pathological line cannot blow the
                // 64 KiB JSON frame limit.
                m.set("text", line.size() > 300 ? line.substr(0, 300) + "..." : line);
                matches.push(std::move(m));
                found++;
            }
            line.clear();
            line_start = offset;
        } else if (ch != '\r') {
            if (line.size() < 4096) line += (char)ch;
        }
    }
    std::fclose(f);

    reply.json.set("lines_scanned", (int64_t)lineno);
    reply.json.set("count", (int64_t)found);
    if (truncated) reply.json.set("truncated", true);
    reply.json.set("matches", std::move(matches));
    return true;
}

}  // namespace handlers
}  // namespace agent
