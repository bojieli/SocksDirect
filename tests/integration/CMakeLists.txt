# Integration tests are driven by pytest. CMake registers per-suite CTest
# entries that delegate to pytest filtered by file. Tests skip themselves
# at runtime if their dependencies (libsd, monitor, ctl) aren't present —
# so a CI runner without RDMA still gets the control-plane suite.

find_package(Python3 COMPONENTS Interpreter)

if(NOT Python3_Interpreter_FOUND)
    message(STATUS "Python3 not found; integration tests will be skipped")
    return()
endif()

# Helper to resolve a target file path that may not exist at configure
# time (e.g. when libsd is gated off).
function(_resolve_target_file var target)
    if(TARGET ${target})
        set(${var} "$<TARGET_FILE:${target}>" PARENT_SCOPE)
    else()
        set(${var} "" PARENT_SCOPE)
    endif()
endfunction()

_resolve_target_file(_libsd_path        sd)
_resolve_target_file(_monitor_path      socksdirect-monitor)
_resolve_target_file(_ctl_path          socksdirect-ctl)

# --- libsd preload suite (uses the new src/lib/ libsd) ----------------------
if(TARGET sd)
    add_test(
        NAME integration-libsd-preload
        COMMAND ${Python3_EXECUTABLE} -m pytest
                ${CMAKE_CURRENT_SOURCE_DIR}/test_libsd_preload.py
                -v --tb=short --color=yes)
    set_tests_properties(integration-libsd-preload PROPERTIES
        TIMEOUT 300
        ENVIRONMENT
            "SOCKSDIRECT_LIB=${_libsd_path};PYTHONDONTWRITEBYTECODE=1")
endif()

# --- ctl suite (no libsd needed) --------------------------------------------
if(TARGET socksdirect-ctl)
    add_test(
        NAME integration-ctl
        COMMAND ${Python3_EXECUTABLE} -m pytest
                ${CMAKE_CURRENT_SOURCE_DIR}/test_socksdirect_ctl.py
                -v --tb=short --color=yes)
    set_tests_properties(integration-ctl PROPERTIES
        TIMEOUT 60
        ENVIRONMENT
            "SOCKSDIRECT_CTL=${_ctl_path};PYTHONDONTWRITEBYTECODE=1")
endif()

# --- monitor daemon end-to-end suite ----------------------------------------
if(TARGET socksdirect-monitor AND TARGET socksdirect-ctl)
    add_test(
        NAME integration-monitor-daemon
        COMMAND ${Python3_EXECUTABLE} -m pytest
                ${CMAKE_CURRENT_SOURCE_DIR}/test_monitor_daemon.py
                -v --tb=short --color=yes)
    set_tests_properties(integration-monitor-daemon PROPERTIES
        TIMEOUT 120
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
endif()

# --- repro CLI suite (pure-python, runs everywhere) -------------------------
add_test(
    NAME integration-repro
    COMMAND ${Python3_EXECUTABLE} -m pytest
            ${CMAKE_CURRENT_SOURCE_DIR}/test_repro_cli.py
            -v --tb=short --color=yes)
set_tests_properties(integration-repro PROPERTIES
    TIMEOUT 60
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

# --- perf-regression CLI suite (pure-python) --------------------------------
add_test(
    NAME integration-perf-regression
    COMMAND ${Python3_EXECUTABLE} -m pytest
            ${CMAKE_CURRENT_SOURCE_DIR}/test_perf_regression.py
            -v --tb=short --color=yes)
set_tests_properties(integration-perf-regression PROPERTIES
    TIMEOUT 60
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

# --- kernel ABI consistency suite (pure-python) -----------------------------
add_test(
    NAME integration-kernel-abi
    COMMAND ${Python3_EXECUTABLE} -m pytest
            ${CMAKE_CURRENT_SOURCE_DIR}/test_kernel_abi_check.py
            -v --tb=short --color=yes)
set_tests_properties(integration-kernel-abi PROPERTIES
    TIMEOUT 30
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

# --- packaging metadata lint -------------------------------------------------
add_test(
    NAME integration-packaging-lint
    COMMAND ${Python3_EXECUTABLE} -m pytest
            ${CMAKE_CURRENT_SOURCE_DIR}/test_packaging_lint.py
            -v --tb=short --color=yes)
set_tests_properties(integration-packaging-lint PROPERTIES
    TIMEOUT 30
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

# --- error-handling-audit drift gate -----------------------------------------
add_test(
    NAME integration-error-handling-audit
    COMMAND ${Python3_EXECUTABLE} -m pytest
            ${CMAKE_CURRENT_SOURCE_DIR}/test_error_handling_audit.py
            -v --tb=short --color=yes)
set_tests_properties(integration-error-handling-audit PROPERTIES
    TIMEOUT 30
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

# --- API.md drift gate -------------------------------------------------------
add_test(
    NAME integration-api-doc-drift
    COMMAND ${Python3_EXECUTABLE} -m pytest
            ${CMAKE_CURRENT_SOURCE_DIR}/test_api_doc_drift.py
            -v --tb=short --color=yes)
set_tests_properties(integration-api-doc-drift PROPERTIES
    TIMEOUT 30
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

# --- docs internal-links + index ---------------------------------------------
add_test(
    NAME integration-docs-links
    COMMAND ${Python3_EXECUTABLE} -m pytest
            ${CMAKE_CURRENT_SOURCE_DIR}/test_docs_links.py
            -v --tb=short --color=yes)
set_tests_properties(integration-docs-links PROPERTIES
    TIMEOUT 30
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

# --- LTP socket conformance --------------------------------------------------
add_test(
    NAME integration-ltp
    COMMAND ${Python3_EXECUTABLE} -m pytest
            ${CMAKE_CURRENT_SOURCE_DIR}/test_ltp.py
            -v --tb=short --color=yes)
set_tests_properties(integration-ltp PROPERTIES
    TIMEOUT 180
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

# --- conformance baseline (no preload; validates coverage.toml) -------------
add_test(
    NAME integration-conformance
    COMMAND ${Python3_EXECUTABLE} -m pytest
            ${CMAKE_CURRENT_SOURCE_DIR}/test_conformance.py
            -v --tb=short --color=yes)
set_tests_properties(integration-conformance PROPERTIES
    TIMEOUT 180
    ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")

# --- microbench smoke (validates JSON output matches the schema) ------------
if(SOCKSDIRECT_BUILD_BENCH)
    add_test(
        NAME integration-bench-smoke
        COMMAND ${Python3_EXECUTABLE} -m pytest
                ${CMAKE_CURRENT_SOURCE_DIR}/test_bench_smoke.py
                -v --tb=short --color=yes)
    set_tests_properties(integration-bench-smoke PROPERTIES
        TIMEOUT 60
        ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1")
endif()
