// SPDX-License-Identifier: Apache-2.0
//
// socksdirect::Config — minimal config loader.
//
// This is the stop-gap loader that lets us strip hardcoded paths/IPs
// from the lib + monitor *now*, without committing to a TOML/YAML
// dependency before Phase 0 finalizes the dep choices. The on-disk
// format is INI-with-sections: each non-comment line is either a
// `[section]` header or a `key = value` pair. Whitespace around `=`
// is ignored; values are returned verbatim.
//
// The header is intentionally header-only and stdlib-only so the
// monitor and the libsd preload library can both pull it in without
// adding link-time deps.
//
// Lookup order:
//   1. SOCKSDIRECT_<SECTION>_<KEY> environment variable (uppercased).
//   2. Section/key from the file pointed to by $SOCKSDIRECT_CONFIG, or
//      /etc/socksdirect/socksdirect.conf if the env var is unset.
//   3. The caller-supplied default.
//
// Errors are NEVER fatal here — parse failures return defaults and emit
// a one-line warning to stderr (callers may redirect via dup2). Callers
// that need stricter behavior should call validate() after construction.

#ifndef SOCKSDIRECT_CONFIG_HPP_
#define SOCKSDIRECT_CONFIG_HPP_

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace socksdirect {

class Config {
public:
    // Default file path; overridden by SOCKSDIRECT_CONFIG.
    static constexpr const char* kDefaultPath = "/etc/socksdirect/socksdirect.conf";

    Config() = default;

    // Load from `path`. If the file is missing or unreadable, the loader
    // proceeds with an empty in-memory state — get_*() will then return
    // env vars (if set) or supplied defaults. This is intentional: a
    // dev environment with no /etc/socksdirect/socksdirect.conf must not
    // crash the library at constructor time.
    static Config load(const std::string& path) {
        Config c;
        std::ifstream fh(path);
        if (!fh) {
            // Not an error — caller may rely entirely on env + defaults.
            return c;
        }
        std::string line;
        std::string section;
        int lineno = 0;
        while (std::getline(fh, line)) {
            ++lineno;
            std::string s = strip(line);
            if (s.empty() || s[0] == '#' || s[0] == ';') continue;
            if (s.front() == '[' && s.back() == ']') {
                section = strip(s.substr(1, s.size() - 2));
                continue;
            }
            auto eq = s.find('=');
            if (eq == std::string::npos) {
                std::fprintf(stderr,
                    "socksdirect: %s:%d: ignored malformed line: %s\n",
                    path.c_str(), lineno, s.c_str());
                continue;
            }
            std::string k = strip(s.substr(0, eq));
            std::string v = strip(s.substr(eq + 1));
            c.values_[section + "." + k] = v;
        }
        return c;
    }

    // Convenience: load from env-or-default path.
    static Config load_default() {
        const char* env_path = std::getenv("SOCKSDIRECT_CONFIG");
        return load(env_path && *env_path ? env_path : kDefaultPath);
    }

    std::string get_string(const std::string& section,
                           const std::string& key,
                           const std::string& fallback) const {
        if (auto v = env_lookup(section, key)) return *v;
        auto it = values_.find(section + "." + key);
        if (it != values_.end()) return it->second;
        return fallback;
    }

    int get_int(const std::string& section,
                const std::string& key,
                int fallback) const {
        std::string s = get_string(section, key, "");
        if (s.empty()) return fallback;
        try { return std::stoi(s); }
        catch (const std::exception&) {
            std::fprintf(stderr,
                "socksdirect: ignored non-integer value for %s.%s: %s\n",
                section.c_str(), key.c_str(), s.c_str());
            return fallback;
        }
    }

    bool get_bool(const std::string& section,
                  const std::string& key,
                  bool fallback) const {
        std::string s = lower(get_string(section, key, ""));
        if (s.empty()) return fallback;
        if (s == "1" || s == "true" || s == "yes" || s == "on")  return true;
        if (s == "0" || s == "false" || s == "no" || s == "off") return false;
        std::fprintf(stderr,
            "socksdirect: ignored non-boolean value for %s.%s: %s\n",
            section.c_str(), key.c_str(), s.c_str());
        return fallback;
    }

    // Return all loaded section.key pairs, for tools/socksdirect-ctl dump.
    const std::map<std::string, std::string>& all() const { return values_; }

    // Throw if any of the listed keys is missing. Useful at monitor
    // startup to surface deployment misconfiguration early.
    void require(const std::vector<std::pair<std::string, std::string>>& keys) const {
        for (const auto& kv : keys) {
            if (env_lookup(kv.first, kv.second)) continue;
            if (values_.count(kv.first + "." + kv.second)) continue;
            throw std::runtime_error(
                "socksdirect: required config missing: " + kv.first + "." + kv.second);
        }
    }

private:
    static std::string strip(const std::string& s) {
        auto a = s.find_first_not_of(" \t\r\n");
        auto b = s.find_last_not_of(" \t\r\n");
        if (a == std::string::npos) return {};
        return s.substr(a, b - a + 1);
    }

    static std::string upper(const std::string& s) {
        std::string r = s;
        for (auto& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return r;
    }

    static std::string lower(const std::string& s) {
        std::string r = s;
        for (auto& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return r;
    }

    // env override: SOCKSDIRECT_<SECTION>_<KEY>, '-' or '.' becomes '_'.
    static std::string env_var_name(const std::string& section,
                                    const std::string& key) {
        std::string n = "SOCKSDIRECT_" + upper(section) + "_" + upper(key);
        for (auto& c : n) if (c == '-' || c == '.') c = '_';
        return n;
    }

    static std::string* leak_string(const std::string& s) {
        // Avoids returning a reference to a temporary; cheap and lifetime-safe.
        thread_local std::string buf;
        buf = s;
        return &buf;
    }

    static std::string* env_lookup(const std::string& section,
                                   const std::string& key) {
        std::string name = env_var_name(section, key);
        const char* v = std::getenv(name.c_str());
        if (!v) return nullptr;
        return leak_string(v);
    }

    std::map<std::string, std::string> values_;
};

}  // namespace socksdirect

#endif  // SOCKSDIRECT_CONFIG_HPP_
