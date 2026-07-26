#include "log.hpp"

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

namespace log {

namespace {
std::string g_path;
Level g_level = Level::Info;
size_t g_max = 256 * 1024;
std::mutex g_mutex;

const char* LevelName(Level l) {
    switch (l) {
        case Level::Error: return "ERROR";
        case Level::Warn: return "WARN";
        case Level::Info: return "INFO";
        case Level::Debug: return "DEBUG";
    }
    return "?";
}
}  // namespace

void Init(const std::string& path, Level level, size_t max_bytes) {
    std::lock_guard<std::mutex> lk(g_mutex);
    g_path = path;
    g_level = level;
    g_max = max_bytes;
    // Truncate on start if oversized, so the file never grows without bound.
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        long sz = std::ftell(f);
        std::fclose(f);
        if (sz < 0 || (size_t)sz > g_max) {
            if (FILE* t = std::fopen(path.c_str(), "wb")) std::fclose(t);
        }
    }
}

void Write(Level level, const char* fmt, ...) {
    if ((int)level > (int)g_level) return;
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_path.empty()) return;

    FILE* f = std::fopen(g_path.c_str(), "ab");
    if (!f) return;

    // Rotate (truncate) if we've hit the cap.
    std::fseek(f, 0, SEEK_END);
    if ((size_t)std::ftell(f) > g_max) {
        std::fclose(f);
        f = std::fopen(g_path.c_str(), "wb");
        if (!f) return;
    }

    std::time_t t = std::time(nullptr);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    std::fprintf(f, "[%s] %-5s ", ts, LevelName(level));

    va_list args;
    va_start(args, fmt);
    std::vfprintf(f, fmt, args);
    va_end(args);

    std::fputc('\n', f);
    std::fclose(f);
}

}  // namespace log
