# ---------------------------------------------------------------------------
# tests/CMakeLists.txt — unit tests, integration glue, and the
# `make check` target.
#
# Unit tests run on every CI; they exercise headers in common/ in isolation
# and don't require RDMA or a kernel module. Integration tests live under
# tests/integration/ and are driven by pytest from CTest — they boot the
# monitor and preloaded clients and assert end-to-end behavior.
# ---------------------------------------------------------------------------

include(CTest)

# --- googletest --------------------------------------------------------------
find_package(GTest QUIET)
if(NOT GTest_FOUND)
    message(STATUS "GTest not found; fetching via FetchContent")
    include(FetchContent)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
        URL_HASH SHA256=8ad598c73ad796e0d8280b082cebd82a630d73e73cd3c70057938a6501bba5d7)
    set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    set(_gtest_target gtest_main)
else()
    set(_gtest_target GTest::gtest_main)
endif()

# --- helpers -----------------------------------------------------------------
function(socksdirect_add_unit_test name)
    set(srcs ${ARGN})
    add_executable(${name} ${srcs})
    target_include_directories(${name} PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(${name} PRIVATE ${_gtest_target} Threads::Threads)
    # Use add_test directly rather than gtest_discover_tests to avoid the
    # post-build discovery step (which costs CI time and adds a fork).
    add_test(NAME ${name} COMMAND ${name} --gtest_color=yes)
    set_tests_properties(${name} PROPERTIES
        TIMEOUT 60
        ENVIRONMENT "LSAN_OPTIONS=suppressions=${CMAKE_SOURCE_DIR}/tests/asan.supp:print_suppressions=0;UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1")
endfunction()

# --- unit tests --------------------------------------------------------------
add_subdirectory(unit)

# --- integration tests (pytest) ----------------------------------------------
# Always added — individual test files skip themselves at runtime if the
# artifact they need (libsd, monitor, ctl, ...) is missing. This way the
# control-plane tests can run on a stock CI runner without RDMA.
add_subdirectory(integration)

# --- convenience target ------------------------------------------------------
add_custom_target(check
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    USES_TERMINAL)
