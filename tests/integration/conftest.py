# Shared pytest fixtures for integration tests.
#
# Each test file that needs a monitor or libsd typically defines its
# own local fixture (see test_monitor_daemon.py / test_libsd_preload.py)
# so per-test concerns (control-socket path, log level, lifecycle
# semantics) are explicit. The module-scope fixtures below are
# convenience wrappers used by a few smaller tests.
#
# Conventions:
#   - Build artifacts are discovered via env vars (set by CMake) or via
#     a glob over `build*/`.
#   - The `monitor` fixture below is intentionally NOT used by
#     test_monitor_daemon.py — that file owns its own monitor
#     lifecycle so parallel ctest runs don't trip over a global pkill.

from __future__ import annotations

import os
import socket
from pathlib import Path

import pytest

# ---------------------------------------------------------------------------
# Discovery of build artifacts
# ---------------------------------------------------------------------------

LIB_PATH_ENV = "SOCKSDIRECT_LIB"
MONITOR_PATH_ENV = "SOCKSDIRECT_MONITOR"


def _resolve_artifact(env_var: str, fallback_globs: list[str]) -> Path | None:
    p = os.environ.get(env_var)
    if p and Path(p).exists():
        return Path(p).resolve()
    repo = Path(__file__).resolve().parents[2]
    for pattern in fallback_globs:
        for hit in repo.glob(pattern):
            if hit.exists():
                return hit.resolve()
    return None


@pytest.fixture(scope="session")
def libsd_path() -> Path:
    p = _resolve_artifact(
        LIB_PATH_ENV,
        # The libsd.so glob covers in-tree and out-of-tree CMake build
        # dirs alike. The pre-rewrite library name was retired in
        # Phase 1; if you hit a stale build with the old name,
        # rebuild from a clean tree.
        ["build/libsd.so", "build*/libsd.so"],
    )
    if p is None:
        pytest.skip(
            f"libsd.so not found; set {LIB_PATH_ENV} or build with "
            f"-DSOCKSDIRECT_WITH_RDMA=ON"
        )
    return p


@pytest.fixture(scope="session")
def monitor_path() -> Path:
    p = _resolve_artifact(
        MONITOR_PATH_ENV,
        ["build/socksdirect-monitor", "build*/socksdirect-monitor"],
    )
    if p is None:
        pytest.skip(
            f"socksdirect-monitor not found; set {MONITOR_PATH_ENV} or "
            f"run cmake --build build -- socksdirect-monitor"
        )
    return p


# ---------------------------------------------------------------------------
# Free port allocator (for non-preload TCP sanity checks)
# ---------------------------------------------------------------------------

@pytest.fixture
def free_tcp_port() -> int:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port
