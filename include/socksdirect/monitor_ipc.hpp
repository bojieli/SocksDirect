// SPDX-License-Identifier: Apache-2.0
//
// socksdirect::MonitorIpc — control-plane wire protocol shared by the
// `socksdirect-monitor` daemon and the `socksdirect-ctl` CLI.
//
// Design choices:
//   - Newline-delimited JSON (NDJSON). One request per line, one response
//     per line, both UTF-8. Keeps debugging trivial (`socat` will do).
//   - Synchronous. Each request gets exactly one response on the same
//     connection, then the connection may either be closed or reused
//     for further requests.
//   - All values are strings or arrays of strings. The control plane is
//     not on the data path — readability beats perf.
//
// We don't pull in a JSON dep. The set of message shapes is small and
// fixed, so we hand-roll a minimal encoder/parser. This header is
// dependency-free except for stdlib.
//
// Wire schema:
//
//   request  : {"op":"<name>","args":["a","b"]}\n
//   response : {"ok":true,"lines":["...","..."]}\n
//            | {"ok":false,"error":"..."}\n
//
// Defined ops (extensible — unknown ops return ok=false, error="unknown op"):
//   - "status"        : monitor lifecycle / pid / uptime
//   - "connections"   : per-connection info
//   - "dump-state"    : verbose internal state for debugging
//   - "reload"        : re-read config (no restart)
//   - "drain"         : stop accepting new clients; finish in-flight
//
// The daemon side is implemented in src/monitor/control.cpp (Phase 2);
// this header contains only the encoder/decoder + a default Unix socket
// path constant so both sides share the truth.

#ifndef SOCKSDIRECT_MONITOR_IPC_HPP_
#define SOCKSDIRECT_MONITOR_IPC_HPP_

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace socksdirect {

// Default Unix socket path. /run/socksdirect/ is created by the package
// install scripts; falling back to /tmp/ for development happens at the
// caller, not here.
constexpr const char* kMonitorCtlSocketDefault = "/run/socksdirect/control.sock";

struct CtlRequest {
    std::string op;
    std::vector<std::string> args;
};

struct CtlResponse {
    bool ok = false;
    std::vector<std::string> lines;
    std::string error;
};

namespace ipc_detail {

inline void json_escape(const std::string& s, std::string& out) {
    out.push_back('"');
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char esc[8];
                    std::snprintf(esc, sizeof(esc), "\\u%04x",
                                  static_cast<unsigned>(static_cast<unsigned char>(c)));
                    out += esc;
                } else {
                    out.push_back(c);
                }
        }
    }
    out.push_back('"');
}

// Skip ASCII whitespace in [s+pos .. s.size()).
inline void skip_ws(const std::string& s, std::size_t& pos) {
    while (pos < s.size() && (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r'))
        ++pos;
}

// Parse a JSON string at pos (expects opening "). Advances pos past closing ".
// Returns false on syntax error.
inline bool parse_string(const std::string& s, std::size_t& pos, std::string& out) {
    if (pos >= s.size() || s[pos] != '"') return false;
    ++pos;
    out.clear();
    while (pos < s.size()) {
        char c = s[pos++];
        if (c == '"') return true;
        if (c == '\\') {
            if (pos >= s.size()) return false;
            char esc = s[pos++];
            switch (esc) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'u':
                    // We accept the escape but only handle \u00XX here. For our
                    // protocol that's enough: we don't pass through anything
                    // with surrogate pairs.
                    if (pos + 4 > s.size()) return false;
                    {
                        unsigned v = 0;
                        for (int i = 0; i < 4; ++i) {
                            char hc = s[pos + i];
                            unsigned d = 0;
                            if (hc >= '0' && hc <= '9') d = hc - '0';
                            else if (hc >= 'a' && hc <= 'f') d = 10 + (hc - 'a');
                            else if (hc >= 'A' && hc <= 'F') d = 10 + (hc - 'A');
                            else return false;
                            v = v * 16 + d;
                        }
                        pos += 4;
                        if (v < 0x80) out.push_back(static_cast<char>(v));
                        else if (v < 0x800) {
                            out.push_back(static_cast<char>(0xC0 | (v >> 6)));
                            out.push_back(static_cast<char>(0x80 | (v & 0x3F)));
                        } else {
                            out.push_back(static_cast<char>(0xE0 | (v >> 12)));
                            out.push_back(static_cast<char>(0x80 | ((v >> 6) & 0x3F)));
                            out.push_back(static_cast<char>(0x80 | (v & 0x3F)));
                        }
                    }
                    break;
                default: return false;
            }
        } else {
            out.push_back(c);
        }
    }
    return false;  // unterminated
}

inline bool parse_string_array(const std::string& s, std::size_t& pos, std::vector<std::string>& out) {
    skip_ws(s, pos);
    if (pos >= s.size() || s[pos] != '[') return false;
    ++pos;
    skip_ws(s, pos);
    out.clear();
    if (pos < s.size() && s[pos] == ']') { ++pos; return true; }
    for (;;) {
        skip_ws(s, pos);
        std::string tmp;
        if (!parse_string(s, pos, tmp)) return false;
        out.push_back(std::move(tmp));
        skip_ws(s, pos);
        if (pos >= s.size()) return false;
        if (s[pos] == ',') { ++pos; continue; }
        if (s[pos] == ']') { ++pos; return true; }
        return false;
    }
}

}  // namespace ipc_detail

inline std::string encode_request(const CtlRequest& req) {
    std::string out;
    out.reserve(64 + req.op.size());
    out += "{\"op\":";
    ipc_detail::json_escape(req.op, out);
    out += ",\"args\":[";
    bool first = true;
    for (const auto& a : req.args) {
        if (!first) out.push_back(',');
        first = false;
        ipc_detail::json_escape(a, out);
    }
    out += "]}\n";
    return out;
}

inline std::string encode_response(const CtlResponse& resp) {
    std::string out;
    out.reserve(64);
    if (resp.ok) {
        out += "{\"ok\":true,\"lines\":[";
        bool first = true;
        for (const auto& l : resp.lines) {
            if (!first) out.push_back(',');
            first = false;
            ipc_detail::json_escape(l, out);
        }
        out += "]}\n";
    } else {
        out += "{\"ok\":false,\"error\":";
        ipc_detail::json_escape(resp.error, out);
        out += "}\n";
    }
    return out;
}

// Both decoders accept a single-line buffer (with or without trailing
// newline). They are case-sensitive on field names and tolerant of
// whitespace. They reject anything they don't recognize.

inline bool decode_request(const std::string& s_in, CtlRequest& out) {
    std::string s = s_in;
    if (!s.empty() && s.back() == '\n') s.pop_back();
    if (!s.empty() && s.back() == '\r') s.pop_back();
    out = CtlRequest{};
    std::size_t pos = 0;
    ipc_detail::skip_ws(s, pos);
    if (pos >= s.size() || s[pos] != '{') return false;
    ++pos;
    bool got_op = false;
    bool got_args = false;
    while (pos < s.size()) {
        ipc_detail::skip_ws(s, pos);
        if (pos < s.size() && s[pos] == '}') { ++pos; break; }
        std::string key;
        if (!ipc_detail::parse_string(s, pos, key)) return false;
        ipc_detail::skip_ws(s, pos);
        if (pos >= s.size() || s[pos] != ':') return false;
        ++pos;
        ipc_detail::skip_ws(s, pos);
        if (key == "op") {
            if (!ipc_detail::parse_string(s, pos, out.op)) return false;
            got_op = true;
        } else if (key == "args") {
            if (!ipc_detail::parse_string_array(s, pos, out.args)) return false;
            got_args = true;
        } else {
            // Unknown key — skip the value (string or array of strings only).
            if (s[pos] == '"') { std::string ign; if (!ipc_detail::parse_string(s, pos, ign)) return false; }
            else if (s[pos] == '[') { std::vector<std::string> ign; if (!ipc_detail::parse_string_array(s, pos, ign)) return false; }
            else return false;
        }
        ipc_detail::skip_ws(s, pos);
        if (pos < s.size() && s[pos] == ',') { ++pos; continue; }
        if (pos < s.size() && s[pos] == '}') { ++pos; break; }
        return false;
    }
    return got_op && got_args;
}

inline bool decode_response(const std::string& s_in, CtlResponse& out) {
    std::string s = s_in;
    if (!s.empty() && s.back() == '\n') s.pop_back();
    if (!s.empty() && s.back() == '\r') s.pop_back();
    out = CtlResponse{};
    std::size_t pos = 0;
    ipc_detail::skip_ws(s, pos);
    if (pos >= s.size() || s[pos] != '{') return false;
    ++pos;
    bool got_ok = false;
    while (pos < s.size()) {
        ipc_detail::skip_ws(s, pos);
        if (pos < s.size() && s[pos] == '}') { ++pos; break; }
        std::string key;
        if (!ipc_detail::parse_string(s, pos, key)) return false;
        ipc_detail::skip_ws(s, pos);
        if (pos >= s.size() || s[pos] != ':') return false;
        ++pos;
        ipc_detail::skip_ws(s, pos);
        if (key == "ok") {
            if (s.compare(pos, 4, "true") == 0)       { out.ok = true;  pos += 4; }
            else if (s.compare(pos, 5, "false") == 0) { out.ok = false; pos += 5; }
            else return false;
            got_ok = true;
        } else if (key == "lines") {
            if (!ipc_detail::parse_string_array(s, pos, out.lines)) return false;
        } else if (key == "error") {
            if (!ipc_detail::parse_string(s, pos, out.error)) return false;
        } else {
            // Unknown key — skip.
            if (s[pos] == '"') { std::string ign; if (!ipc_detail::parse_string(s, pos, ign)) return false; }
            else if (s[pos] == '[') { std::vector<std::string> ign; if (!ipc_detail::parse_string_array(s, pos, ign)) return false; }
            else if (s.compare(pos, 4, "true") == 0) pos += 4;
            else if (s.compare(pos, 5, "false") == 0) pos += 5;
            else if (s.compare(pos, 4, "null") == 0) pos += 4;
            else return false;
        }
        ipc_detail::skip_ws(s, pos);
        if (pos < s.size() && s[pos] == ',') { ++pos; continue; }
        if (pos < s.size() && s[pos] == '}') { ++pos; break; }
        return false;
    }
    return got_ok;
}

// ---------------------------------------------------------------------------
// I/O helpers — small wrappers around the Unix-domain socket primitives.
// They are blocking and EINTR-resilient, intended for the CLI tool. The
// daemon uses an event loop and these aren't on its hot path.
// ---------------------------------------------------------------------------

// Read until '\n' or EOF or error. Returns the bytes read (excluding the '\n').
// On EOF before any newline the returned string is empty and `eof_out` is true.
inline std::string read_line(int fd, bool* eof_out = nullptr) {
    std::string out;
    if (eof_out) *eof_out = false;
    for (;;) {
        char c;
        ssize_t n = ::read(fd, &c, 1);
        if (n == 1) {
            if (c == '\n') return out;
            out.push_back(c);
            // Pathological input: cap the line length.
            if (out.size() > (1 << 20)) return out;
        } else if (n == 0) {
            if (eof_out) *eof_out = true;
            return out;
        } else {
            if (errno == EINTR) continue;
            if (eof_out) *eof_out = true;
            return out;
        }
    }
}

inline bool write_all(int fd, const std::string& s) {
    const char* p = s.data();
    std::size_t left = s.size();
    while (left > 0) {
        ssize_t n = ::write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += n;
        left -= static_cast<std::size_t>(n);
    }
    return true;
}

// Connect to a Unix-domain SOCK_STREAM at `path`. Returns the fd on
// success; -1 on failure (errno set).
inline int connect_unix(const std::string& path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() + 1 > sizeof(addr.sun_path)) {
        errno = ENAMETOOLONG;
        ::close(fd);
        return -1;
    }
    std::memcpy(addr.sun_path, path.data(), path.size());
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        int saved = errno;
        ::close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

// Open and listen on a Unix-domain SOCK_STREAM. Removes any existing
// socket at `path` first (idempotent boot). Returns fd on success.
inline int listen_unix(const std::string& path, int backlog = 16) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path.size() + 1 > sizeof(addr.sun_path)) {
        errno = ENAMETOOLONG;
        ::close(fd);
        return -1;
    }
    std::memcpy(addr.sun_path, path.data(), path.size());
    ::unlink(path.c_str());
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        int saved = errno;
        ::close(fd);
        errno = saved;
        return -1;
    }
    if (::listen(fd, backlog) < 0) {
        int saved = errno;
        ::close(fd);
        ::unlink(path.c_str());
        errno = saved;
        return -1;
    }
    return fd;
}

}  // namespace socksdirect

#endif  // SOCKSDIRECT_MONITOR_IPC_HPP_
