// SPDX-License-Identifier: Apache-2.0
//
// Internal: glibc-shim helpers used by every src/lib/ translation unit.
//
// The preload library replaces a small set of libc functions. For each
// hook, we need:
//   - the real glibc symbol (dlsym(RTLD_NEXT, ...)) so we can forward.
//   - a hook function with C linkage that the dynamic linker binds to.
//
// The DECLARE_REAL / RESOLVE_REAL macros below cache the next-symbol
// pointer in a function-local static so we pay the dlsym cost exactly
// once per symbol per process. Unlike a global init list, this defers
// the lookup until first use, which avoids ordering hazards during
// LD_PRELOAD setup.
//
// All hooks share a single "is libsd active?" guard. When the guard is
// false (e.g. inside a constructor that's still bootstrapping), every
// hook just forwards to glibc. This is also how we break re-entrancy
// when the hook itself calls one of its own peers.

#ifndef SOCKSDIRECT_LIB_INTERCEPT_HPP_
#define SOCKSDIRECT_LIB_INTERCEPT_HPP_

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>

namespace socksdirect {
namespace preload {

// True after preload_init() has run. Hooks short-circuit to glibc until
// then so we don't recurse during bootstrap.
extern std::atomic<bool> g_active;

// Re-entrancy guard. Hooks set this thread-local before doing any work
// that could re-enter another hook (e.g. emitting metrics that
// internally call write()). When set, every hook forwards directly to
// glibc.
extern thread_local bool g_in_hook;

struct ScopedReentrancyGuard {
    bool prev;
    ScopedReentrancyGuard() : prev(g_in_hook) { g_in_hook = true; }
    ~ScopedReentrancyGuard()                  { g_in_hook = prev; }
};

inline void* resolve_symbol(const char* name) {
    void* p = ::dlsym(RTLD_NEXT, name);
    if (!p) {
        // Cannot LOG_* here — the logger may not be initialized.
        std::fprintf(stderr, "socksdirect: dlsym(%s) failed: %s\n",
                     name, ::dlerror());
        std::abort();
    }
    return p;
}

}  // namespace preload
}  // namespace socksdirect

// Declare a function-static-cached glibc fallback pointer for `name`.
// Usage:
//   DECLARE_REAL(int, socket, int, int, int);
//   ...
//   int rc = REAL(socket)(domain, type, protocol);
#define DECLARE_REAL(ret, name, ...)                                           \
    using real_##name##_t = ret(*)(__VA_ARGS__);                               \
    inline real_##name##_t real_##name() {                                     \
        static real_##name##_t p = reinterpret_cast<real_##name##_t>(          \
            ::socksdirect::preload::resolve_symbol(#name));                    \
        return p;                                                              \
    }

#define REAL(name) real_##name()

// All public hooks must be extern "C" so the dynamic linker can match
// them. SOCKSDIRECT_HOOK is a small wrapper to prevent name-mangling.
#define SOCKSDIRECT_HOOK extern "C"

#endif  // SOCKSDIRECT_LIB_INTERCEPT_HPP_
