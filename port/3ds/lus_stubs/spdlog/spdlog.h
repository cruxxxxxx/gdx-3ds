/* Spike stub: no-op spdlog surface for the LUS carve compile gate. */
#pragma once
#include <string>
#include <memory>

#define SPDLOG_TRACE(...) ((void)0)
#define SPDLOG_DEBUG(...) ((void)0)
#define SPDLOG_INFO(...) ((void)0)
#define SPDLOG_WARN(...) ((void)0)
#define SPDLOG_ERROR(...) ((void)0)
#define SPDLOG_CRITICAL(...) ((void)0)

namespace spdlog {
namespace level {
enum level_enum { trace = 0, debug, info, warn, err, critical, off, n_levels };
}
class logger {
  public:
    template <typename... Args> void trace(Args&&...) {
    }
    template <typename... Args> void debug(Args&&...) {
    }
    template <typename... Args> void info(Args&&...) {
    }
    template <typename... Args> void warn(Args&&...) {
    }
    template <typename... Args> void error(Args&&...) {
    }
    template <typename... Args> void critical(Args&&...) {
    }
    void set_level(level::level_enum) {
    }
    void flush() {
    }
};
inline std::shared_ptr<logger> get(const std::string&) {
    return nullptr;
}
inline std::shared_ptr<logger> default_logger() {
    return nullptr;
}
inline void set_level(level::level_enum) {
}
template <typename... Args> inline void trace(Args&&...) {
}
template <typename... Args> inline void debug(Args&&...) {
}
template <typename... Args> inline void info(Args&&...) {
}
template <typename... Args> inline void warn(Args&&...) {
}
template <typename... Args> inline void error(Args&&...) {
}
template <typename... Args> inline void critical(Args&&...) {
}
} // namespace spdlog
