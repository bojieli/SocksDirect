# Shared pytest fixtures for integration tests.
#
# Conventions:
#   - Tests assume the monitor and libsd have been built and that paths
#     are passed via SOCKSDIRECT_LIB and SOCKSDIRECT_MONITOR env vars
#     (set by the CMake test target).
#   - Each test function gets a fresh monitor process via the `monitor`
#     fixture. The monitor is killed after the test.
#   - Helper utilities for spawning preloaded child processes are in
#     tests/integration/_harness.py.

from __future__ import annotations

import os
import shutil
import socket
import subprocess
import time
from pathlib import Path
from typing import Iterator

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
        # dirs alike. The historical `libipc.so` name was retired in
        # Phase 1 of the rewrite; if you hit a stale build with that
        # name, rebuild from a clean tree.
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
        ["build/socksdirect-monitor", "build*/socksdirect-monitor", "build*/monitor"],
    )
    if p is None:
        pytest.skip(
            f"socksdirect-monitor not found; set {MONITOR_PATH_ENV} or build "
            f"with -DSOCKSDIRECT_WITH_RDMA=ON"
        )
    return p


# ---------------------------------------------------------------------------
# Monitor lifecycle
# ---------------------------------------------------------------------------

MONITOR_SOCKET = Path("/tmp/ipcd.sock")


@pytest.fixture
def monitor(monitor_path: Path) -> Iterator[subprocess.Popen]:
    """Boot a fresh monitor; tear it down after the test."""
    # Make sure no stale socket from a prior run lingers.
    MONITOR_SOCKET.unlink(missing_ok=True)

    # Make sure no stale monitor is still running.
    subprocess.run(["pkill", "-f", "socksdirect-monitor"], check=False)
    subprocess.run(["pkill", "-f", "/monitor"], check=False)
    time.sleep(0.05)

    proc = subprocess.Popen(
        [str(monitor_path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        cwd="/tmp",
    )

    # Wait up to 3 s for the monitor to create its accept socket.
    deadline = time.time() + 3.0
    while time.time() < deadline:
        if MONITOR_SOCKET.exists():
            break
        if proc.poll() is not None:
            stdout = proc.stdout.read().decode(errors="replace")  # type: ignore[union-attr]
            stderr = proc.stderr.read().decode(errors="replace")  # type: ignore[union-attr]
            pytest.fail(
                f"monitor exited prematurely (rc={proc.returncode})\n"
                f"stdout:\n{stdout}\nstderr:\n{stderr}"
            )
        time.sleep(0.05)
    else:
        proc.terminate()
        pytest.fail("monitor did not create its accept socket within 3 s")

    yield proc

    proc.terminate()
    try:
        proc.wait(timeout=2.0)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()


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
