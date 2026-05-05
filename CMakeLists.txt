cmake_minimum_required(VERSION 3.16)
project(socksdirect
        VERSION 0.1.0
        DESCRIPTION "Fast and compatible sockets in user space"
        LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Standards and global flags
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Hardening on by default for release; can be disabled for prototyping.
option(SOCKSDIRECT_HARDEN "Enable compiler hardening flags" ON)
option(SOCKSDIRECT_BUILD_TESTS "Build unit + integration tests" ON)
option(SOCKSDIRECT_BUILD_BENCH "Build microbenchmark binaries" ON)
option(SOCKSDIRECT_BUILD_LEGACY "Build legacy smoke programs in test/" OFF)
option(SOCKSDIRECT_WITH_RDMA "Build RDMA inter-host transport (requires Mellanox OFED for HERD path)" OFF)
option(SOCKSDIRECT_WITH_HERD "Build the HERD helper library (requires Mellanox OFED experimental verbs)" OFF)
option(SOCKSDIRECT_WITH_ASAN "Build with AddressSanitizer" OFF)
option(SOCKSDIRECT_WITH_TSAN "Build with ThreadSanitizer" OFF)
option(SOCKSDIRECT_WITH_UBSAN "Build with UndefinedBehaviorSanitizer" OFF)

set(_warn_flags "-Wall -Wextra")
if(SOCKSDIRECT_HARDEN)
    set(_warn_flags "${_warn_flags} -fstack-protector-strong -D_FORTIFY_SOURCE=2")
endif()
set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} -g ${_warn_flags}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g ${_warn_flags}")

if(SOCKSDIRECT_WITH_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()
if(SOCKSDIRECT_WITH_TSAN)
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
endif()
if(SOCKSDIRECT_WITH_UBSAN)
    add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=undefined)
endif()

if(COMMAND cmake_policy)
    cmake_policy(SET CMP0003 NEW)
endif()

# ---------------------------------------------------------------------------
# Optional dependencies
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)

if(SOCKSDIRECT_WITH_RDMA OR SOCKSDIRECT_WITH_HERD)
    find_library(IBVERBS_LIBRARY NAMES ibverbs)
    if(NOT IBVERBS_LIBRARY)
        message(FATAL_ERROR "ibverbs requested but libibverbs not found. apt install libibverbs-dev")
    endif()
endif()

# ---------------------------------------------------------------------------
# HERD helper library (requires Mellanox OFED experimental verbs)
# ---------------------------------------------------------------------------
if(SOCKSDIRECT_WITH_HERD)
    add_library(hrd SHARED rdma/hrd_conn.cc rdma/hrd_util.cc)
    target_link_libraries(hrd PUBLIC ${IBVERBS_LIBRARY} memcached numa Threads::Threads)
    target_include_directories(hrd PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/rdma)
endif()

# ---------------------------------------------------------------------------
# libsd (the preloaded user-space library)
# ---------------------------------------------------------------------------
if(SOCKSDIRECT_WITH_RDMA AND SOCKSDIRECT_WITH_HERD)
    set(LIBSD_SOURCES
        common/helper.c
        common/interprocess_t.cpp
        common/rdma.cpp
        lib/setup_sock_lib.cpp
        lib/socket_lib.cpp
        lib/file_lib.cpp
        lib/poll_lib.cpp
        lib/lib.cpp
        lib/fork.cpp
        lib/rdma_lib.cpp)
    add_library(sd SHARED ${LIBSD_SOURCES})
    set_target_properties(sd PROPERTIES OUTPUT_NAME "sd")
    target_link_libraries(sd PUBLIC hrd dl Threads::Threads ${IBVERBS_LIBRARY})
    target_include_directories(sd PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}/include)

    add_library(sd-common SHARED common/helper.c lib/pot_socket_lib.cpp common/rdma.cpp)
    set_target_properties(sd-common PROPERTIES OUTPUT_NAME "sdcommon")
    target_link_libraries(sd-common PUBLIC hrd dl Threads::Threads ${IBVERBS_LIBRARY})

    # Backwards-compatible aliases until callers are migrated.
    add_library(ipc ALIAS sd)
    add_library(ipc-common ALIAS sd-common)
endif()

# ---------------------------------------------------------------------------
# Monitor daemon (production, post-rewrite)
# ---------------------------------------------------------------------------
# The new daemon under src/monitor/ uses the new Logger / Metrics /
# MonitorIpc / Config and is built unconditionally. It implements the
# control plane that socksdirect-ctl(1) talks to and the daemon
# lifecycle bits (signalfd, sd_notify, pid file). The data-plane
# wiring lands in Phase 3.
add_executable(socksdirect-monitor src/monitor/main.cpp)
target_include_directories(socksdirect-monitor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(socksdirect-monitor PRIVATE Threads::Threads)
install(TARGETS socksdirect-monitor RUNTIME DESTINATION sbin)

# Legacy data-plane monitor (RDMA bootstrap) preserved for the libsd
# build path during the migration. Renamed to avoid colliding with the
# new daemon.
if(SOCKSDIRECT_WITH_RDMA)
    add_executable(socksdirect-monitor-legacy
        common/helper.c
        common/interprocess_t.cpp
        common/rdma.cpp
        monitor/setup_sock_monitor.cpp
        monitor/process.cpp
        monitor/sock_monitor.cpp
        monitor/main.cpp
        monitor/rdma_monitor.cpp)
    target_link_libraries(socksdirect-monitor-legacy PRIVATE Threads::Threads ${IBVERBS_LIBRARY})
    target_include_directories(socksdirect-monitor-legacy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endif()

# ---------------------------------------------------------------------------
# Apps — small demo / reproduction binaries (built unconditionally;
# they're plain BSD socket programs and need no RDMA).
# ---------------------------------------------------------------------------
add_subdirectory(apps/rpclib-demo)

# ---------------------------------------------------------------------------
# socksdirect-ctl — control-plane CLI talking to the monitor daemon.
# Builds without RDMA/HERD (it's a thin wrapper around the IPC protocol).
# ---------------------------------------------------------------------------
add_executable(socksdirect-ctl tools/socksdirect-ctl.cpp)
target_include_directories(socksdirect-ctl PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(socksdirect-ctl PRIVATE Threads::Threads)
install(TARGETS socksdirect-ctl RUNTIME DESTINATION bin)

# ---------------------------------------------------------------------------
# Unit + integration test target
# ---------------------------------------------------------------------------
if(SOCKSDIRECT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Microbenchmark binaries
#   - bench/microbench/ runs without libsd/RDMA (CI perf-regression gate)
#   - bench/intra/ and bench/inter/ require libsd + RDMA and are added
#     transitively from bench/CMakeLists.txt only when those are present.
# ---------------------------------------------------------------------------
if(SOCKSDIRECT_BUILD_BENCH)
    add_subdirectory(bench/microbench)
    add_subdirectory(bench/baselines)
endif()
if(SOCKSDIRECT_BUILD_BENCH AND SOCKSDIRECT_WITH_RDMA AND SOCKSDIRECT_WITH_HERD)
    add_subdirectory(bench)
endif()

# ---------------------------------------------------------------------------
# Legacy smoke programs (off by default; preserved for blame archaeology)
# ---------------------------------------------------------------------------
if(SOCKSDIRECT_BUILD_LEGACY AND SOCKSDIRECT_WITH_RDMA AND SOCKSDIRECT_WITH_HERD)
    include(cmake/legacy_smoke_programs.cmake)
endif()

# ---------------------------------------------------------------------------
# Reporting
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "socksdirect build configuration:")
message(STATUS "  Build type:      ${CMAKE_BUILD_TYPE}")
message(STATUS "  Tests:           ${SOCKSDIRECT_BUILD_TESTS}")
message(STATUS "  Benchmarks:      ${SOCKSDIRECT_BUILD_BENCH}")
message(STATUS "  Legacy smoke:    ${SOCKSDIRECT_BUILD_LEGACY}")
message(STATUS "  RDMA transport:  ${SOCKSDIRECT_WITH_RDMA}")
message(STATUS "  HERD library:    ${SOCKSDIRECT_WITH_HERD}")
message(STATUS "  ASan/TSan/UBSan: ${SOCKSDIRECT_WITH_ASAN}/${SOCKSDIRECT_WITH_TSAN}/${SOCKSDIRECT_WITH_UBSAN}")
message(STATUS "")
