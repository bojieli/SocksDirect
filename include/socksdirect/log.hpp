// SPDX-License-Identifier: Apache-2.0
//
// socksdirect::Logger — leveled, header-only logging.
//
// Replaces the printf-based DEBUG/FATAL/ERROR macros currently scattered
// across lib/ and monitor/. Header-only so the preload library and the
// monitor daemon can both pull it in without adding a link-time dep.
//
// Conventions:
//   - Level chosen at runtime via SOCKSDIRECT_LOG=trace|debug|info|warn|error.
//     Anything below the chosen level is dropped before any formatting work.
//   - Output goes to a single sink configured at boot:
//       sink=stderr  (default for the preload library)
//       sink=file    (path = SOCKSDIRECT_LOG_FILE or /var/log/socksdirect/<pid>.log
//                     for the monitor)
//   - Each line is structured: ISO-8601 timestamp, level, pid, tid, then the
//     formatted message. No JSON because grep is the lingua franca here, but
//     the field order is stable so it can be parsed.
//
// Why no spdlog: this stop-gap header keeps the dependency graph empty for
// CI on stock Ubuntu. When the project takes its first heavy dep we'll swap
// the body out behind the same `LOG_*` macros. Until then, this is enough
// for production: it has level filtering, structured fields, file rotation
// support via logrotate(8), and is thread-safe.

#ifndef SOCKSDIRECT_LOG_HPP_
#define SOCKSDIRECT_LOG_HPP_

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <strings.h>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/syscall.h>
#endif

namespace socksdirect {

enum LogLevel : int {
    kLogTrace = 0,
    kLogDebug = 1,
    kLogInfo  = 2,
    kLogWarn  = 3,
    kLogError = 4,
    kLogOff   = 5,
};

inline const char* level_name(LogLevel l) {
    switch (l) {
        case kLogTrace: return "trace";
        case kLogDebug: return "debug";
        case kLogInfo:  return "info";
        case kLogWarn:  return "warn";
        case kLogError: return "error";
        case kLogOff:   return "off";
    }
    return "?";
}

inline LogLevel parse_level(const char* s, LogLevel fallback = kLogInfo) {
    if (!s) return fallback;
    // Case-insensitive, accept the first letter as an alias.
    char head = static_cast<char>(::tolower(static_cast<unsigned char>(s[0])));
    if (!::strcasecmp(s, "trace") || head == 't') return kLogTrace;
    if (!::strcasecmp(s, "debug") || head == 'd') return kLogDebug;
    if (!::strcasecmp(s, "info")  || head == 'i') return kLogInfo;
    if (!::strcasecmp(s, "warn")  || !::strcasecmp(s, "warning") || head == 'w')
        return kLogWarn;
    if (!::strcasecmp(s, "error") || head == 'e') return kLogError;
    if (!::strcasecmp(s, "off")   || head == 'o') return kLogOff;
    return fallback;
}

class Logger {
public:
    // Test seam. Production code uses Logger::instance().
    Logger() = default;

    static Logger& instance() {
        static Logger g;
        static std::once_flag init;
        std::call_once(init, [&]() {
            g.configure_from_env();
        });
        return g;
    }

    void set_level(LogLevel l) { level_.store(static_cast<int>(l), std::memory_order_relaxed); }
    LogLevel level() const { return static_cast<LogLevel>(level_.load(std::memory_order_relaxed)); }

    // Returns false on failure; the previous sink is preserved. Path "" or
    // "stderr" reverts to stderr.
    bool set_sink_file(const std::string& path) {
        std::lock_guard<std::mutex> g(mu_);
        // Close only sinks we own (opened ourselves). FILEs supplied by
        // tests via set_sink_FILE are borrowed; closing them here would
        // double-free.
        close_owned_sink_locked();
        if (path.empty() || path == "stderr") {
            sink_ = stderr;
            sink_path_.clear();
            return true;
        }
        FILE* f = std::fopen(path.c_str(), "ae");  // append, close-on-exec
        if (!f) {
            sink_ = stderr;
            sink_path_.clear();
            return false;
        }
        std::setvbuf(f, nullptr, _IOLBF, 0);  // line-buffered
        sink_ = f;
        sink_owned_ = true;
        sink_path_ = path;
        return true;
    }

    const std::string& sink_path() const { return sink_path_; }

    bool enabled(LogLevel l) const {
        return static_cast<int>(l) >= level_.load(std::memory_order_relaxed);
    }

    // Vararg variant; LOG_* macros forward here. The `enabled()` check is
    // duplicated in the macro so the formatting cost is paid only when the
    // line is going to be emitted.
    void log(LogLevel l, const char* file, int line,
             const char* fmt, ...) __attribute__((format(printf, 5, 6))) {
        if (!enabled(l)) return;
        va_list ap;
        va_start(ap, fmt);
        vlog(l, file, line, fmt, ap);
        va_end(ap);
    }

    void vlog(LogLevel l, const char* file, int line,
              const char* fmt, va_list ap) {
        char body[1024];
        int n = std::vsnprintf(body, sizeof(body), fmt, ap);
        if (n < 0) return;
        if (static_cast<std::size_t>(n) >= sizeof(body)) {
            // Truncated; tag it so the reader knows.
            std::snprintf(body + sizeof(body) - 5, 5, "...");
        }

        // Timestamp without locale, no malloc.
        // 4+1+2+1+2+ 'T' +2+1+2+1+2+'.'+6+'Z'+'\0' = 28; oversized for slack.
        char ts[64];
        timespec tp{};
        clock_gettime(CLOCK_REALTIME, &tp);
        std::tm tm{};
        ::gmtime_r(&tp.tv_sec, &tm);
        std::snprintf(ts, sizeof(ts),
                      "%04d-%02d-%02dT%02d:%02d:%02d.%06ldZ",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                      tm.tm_hour, tm.tm_min, tm.tm_sec,
                      static_cast<long>(tp.tv_nsec / 1000));

        // Strip path prefix from the file location for terser logs.
        const char* base = std::strrchr(file, '/');
        const char* loc  = base ? base + 1 : file;

        std::lock_guard<std::mutex> g(mu_);
        FILE* sink = sink_ ? sink_ : stderr;
        std::fprintf(sink,
            "%s %-5s pid=%d tid=%lld %s:%d  %s\n",
            ts, level_name(l), static_cast<int>(getpid()),
            static_cast<long long>(gettid_self()), loc, line, body);
    }

    // Used by tests to redirect output to a memory buffer-backed FILE*.
    // The FILE is borrowed — Logger never closes it.
    void set_sink_FILE(FILE* f) {
        std::lock_guard<std::mutex> g(mu_);
        close_owned_sink_locked();
        sink_ = f ? f : stderr;
        sink_owned_ = false;
        sink_path_.clear();
    }

    ~Logger() {
        std::lock_guard<std::mutex> g(mu_);
        close_owned_sink_locked();
    }

private:
    void configure_from_env() {
        const char* lvl = std::getenv("SOCKSDIRECT_LOG");
        set_level(parse_level(lvl, kLogInfo));
        const char* path = std::getenv("SOCKSDIRECT_LOG_FILE");
        if (path && *path) set_sink_file(path);
        else set_sink_FILE(stderr);
    }

    static long long gettid_self() {
#if defined(SYS_gettid)
        return static_cast<long long>(syscall(SYS_gettid));
#else
        return static_cast<long long>(0);
#endif
    }

    void close_owned_sink_locked() {
        if (sink_ && sink_owned_ && sink_ != stderr) {
            std::fclose(sink_);
        }
        sink_ = nullptr;
        sink_owned_ = false;
    }

    std::atomic<int> level_{kLogInfo};
    std::mutex mu_;
    FILE* sink_ = nullptr;
    bool sink_owned_ = false;
    std::string sink_path_;
};

}  // namespace socksdirect

// Macros pay the format cost only when the line would actually be emitted.
#define SOCKSDIRECT_LOG(level, ...)                                            \
    do {                                                                       \
        auto& _lg_ = ::socksdirect::Logger::instance();                        \
        if (_lg_.enabled(level)) _lg_.log(level, __FILE__, __LINE__, __VA_ARGS__); \
    } while (0)

#define LOG_TRACE(...) SOCKSDIRECT_LOG(::socksdirect::kLogTrace, __VA_ARGS__)
#define LOG_DEBUG(...) SOCKSDIRECT_LOG(::socksdirect::kLogDebug, __VA_ARGS__)
#define LOG_INFO(...)  SOCKSDIRECT_LOG(::socksdirect::kLogInfo,  __VA_ARGS__)
#define LOG_WARN(...)  SOCKSDIRECT_LOG(::socksdirect::kLogWarn,  __VA_ARGS__)
#define LOG_ERROR(...) SOCKSDIRECT_LOG(::socksdirect::kLogError, __VA_ARGS__)

#endif  // SOCKSDIRECT_LOG_HPP_
