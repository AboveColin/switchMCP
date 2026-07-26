// Album access: list and download screenshots/videos from the capture album.
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
const char* ContentName(u8 c) {
    switch (c) {
        case CapsAlbumFileContents_ScreenShot: return "screenshot";
        case CapsAlbumFileContents_Movie: return "movie";
        case CapsAlbumFileContents_ExtraScreenShot: return "extra_screenshot";
        case CapsAlbumFileContents_ExtraMovie: return "extra_movie";
    }
    return "unknown";
}
}  // namespace

bool AlbumList(const Request& req, Reply& reply) {
    // Default to SD storage; NAND album also available via storage=0.
    CapsAlbumStorage storage =
        (CapsAlbumStorage)req["storage"].as_int(CapsAlbumStorage_Sd);
    u64 count = 0;
    if (R_FAILED(capsaGetAlbumFileCount(storage, &count)))
        return Fail(reply, "caps_failed", "cannot count album files");
    if (count > 1000) count = 1000;  // sanity cap

    std::vector<CapsAlbumEntry> entries(count);
    u64 got = 0;
    if (count && R_FAILED(capsaGetAlbumFileList(storage, &got, entries.data(), count)))
        return Fail(reply, "caps_failed", "cannot list album files");

    json::Value list = json::Value::array();
    for (u64 i = 0; i < got; i++) {
        const CapsAlbumFileId& id = entries[i].file_id;
        const CapsAlbumFileDateTime& d = id.datetime;
        json::Value e = json::Value::object();
        e.set("size", (int64_t)entries[i].size);
        char tid[17];
        std::snprintf(tid, sizeof(tid), "%016lx", (unsigned long)id.application_id);
        e.set("application_id", tid);
        e.set("content", ContentName(id.content));
        e.set("storage", (int64_t)id.storage);
        // datetime fields double as the file's address for album_download.
        e.set("year", (int64_t)d.year);
        e.set("month", (int64_t)d.month);
        e.set("day", (int64_t)d.day);
        e.set("hour", (int64_t)d.hour);
        e.set("minute", (int64_t)d.minute);
        e.set("second", (int64_t)d.second);
        e.set("id", (int64_t)d.id);
        char ts[24];
        std::snprintf(ts, sizeof(ts), "%04u-%02u-%02u %02u:%02u:%02u", d.year, d.month,
                      d.day, d.hour, d.minute, d.second);
        e.set("datetime", ts);
        list.push(std::move(e));
    }
    reply.json.set("files", std::move(list));
    reply.json.set("count", (int64_t)got);
    return true;
}

bool AlbumDownload(const Request& req, Reply& reply) {
    // Rebuild the file id from fields returned by album_list.
    CapsAlbumFileId id{};
    id.application_id =
        std::strtoull(req["application_id"].as_string("0").c_str(), nullptr, 16);
    id.storage = (u8)req["storage"].as_int(CapsAlbumStorage_Sd);
    id.content = (u8)req["content_type"].as_int(CapsAlbumFileContents_ScreenShot);
    id.datetime.year = (u16)req["year"].as_int(0);
    id.datetime.month = (u8)req["month"].as_int(0);
    id.datetime.day = (u8)req["day"].as_int(0);
    id.datetime.hour = (u8)req["hour"].as_int(0);
    id.datetime.minute = (u8)req["minute"].as_int(0);
    id.datetime.second = (u8)req["second"].as_int(0);
    id.datetime.id = (u8)req["id"].as_int(0);

    // Screenshots (JPEG) fit comfortably; movies exceed the 1 MiB frame cap and
    // are reported as too large rather than truncated.
    constexpr size_t kMax = 1u << 20;
    std::vector<uint8_t> buf(kMax);
    u64 out_size = 0;
    Result rc = capsaLoadAlbumFile(&id, &out_size, buf.data(), buf.size());
    if (R_FAILED(rc)) {
        if (id.content == CapsAlbumFileContents_Movie)
            return Fail(reply, "too_large", "movies exceed the 1 MiB transfer cap");
        return Fail(reply, "caps_failed", "cannot load album file");
    }
    buf.resize(out_size);
    reply.out = std::move(buf);
    reply.json.set("jpeg", id.content == CapsAlbumFileContents_ScreenShot);
    return true;
}

}  // namespace handlers
}  // namespace agent
