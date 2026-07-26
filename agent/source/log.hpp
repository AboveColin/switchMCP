// Size-capped file logging for switch-agentd. Never fatals; logging failures
// are silently ignored so they can't take down the console.
#pragma once

#include <string>

namespace log {

enum class Level { Error, Warn, Info, Debug };

// Opens (creates) the log file, truncating if it already exceeds max_bytes.
void Init(const std::string& path, Level level, size_t max_bytes = 256 * 1024);

void Write(Level level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

#define LOG_ERROR(...) ::log::Write(::log::Level::Error, __VA_ARGS__)
#define LOG_WARN(...) ::log::Write(::log::Level::Warn, __VA_ARGS__)
#define LOG_INFO(...) ::log::Write(::log::Level::Info, __VA_ARGS__)
#define LOG_DEBUG(...) ::log::Write(::log::Level::Debug, __VA_ARGS__)

}  // namespace log
