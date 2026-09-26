#pragma once

#include <string_view>

namespace uwb::server {

// Optional logging sink (implementation plan §16.1). The server core never
// formats or stores log lines itself; the host application or firmware decides
// where logs go.
enum class LogLevel : std::uint8_t {
    Debug = 0,
    Info = 1,
    Warn = 2,
    Error = 3,
};

class ILogger {
public:
    virtual ~ILogger() = default;

    virtual void log(LogLevel level, std::string_view message) noexcept = 0;

    void debug(std::string_view message) noexcept { log(LogLevel::Debug, message); }
    void info(std::string_view message) noexcept { log(LogLevel::Info, message); }
    void warn(std::string_view message) noexcept { log(LogLevel::Warn, message); }
    void error(std::string_view message) noexcept { log(LogLevel::Error, message); }
};

} // namespace uwb::server
