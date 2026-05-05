"""Intra-host TCP echo under libsd preload.

This is the canonical end-to-end correctness test: two preloaded
processes establish a loopback TCP connection through the monitor, the
client sends N messages, the server echoes them, and the client verifies
that what came back matches what was sent.

Currently this is a SKELETON test — it shells out to the legacy
test_sock_server / test_sock_client smoke programs from the prototype
and asserts that they exit cleanly. As Phase 3 lands the new socket
API, this test will be expanded with assertions on individual semantics
(EAGAIN behavior, partial reads, EPIPE on closed peer, etc.).
"""

from __future__ import annotations

import os
import time
from pathlib import Path

import pytest

from _harness import preloaded, run_preloaded


def _find_legacy_binary(name: str) -> Path | None:
    repo = Path(__file__).resolve().parents[2]
    for hit in repo.glob(f"build*/legacy_{name}"):
        if hit.exists():
            return hit
    for hit in repo.glob(f"build*/{name}"):
        if hit.exists():
            return hit
    return None


@pytest.mark.skipif(
    _find_legacy_binary("test_sock_server") is None,
    reason="legacy smoke binaries not built (set -DSOCKSDIRECT_BUILD_LEGACY=ON)",
)
def test_legacy_smoke_pingpong(libsd_path: Path, monitor):
    server_bin = _find_legacy_binary("test_sock_pingpong_server")
    client_bin = _find_legacy_binary("test_sock_pingpong_client")
    if server_bin is None or client_bin is None:
        pytest.skip("ping-pong binaries not built")

    server = preloaded(libsd_path, [str(server_bin)])
    time.sleep(0.5)
    try:
        r = run_preloaded(libsd_path, [str(client_bin)], timeout=10)
        assert r.returncode == 0, (
            f"client exited {r.returncode}; stderr: {r.stderr.decode(errors='replace')}"
        )
    finally:
        server.terminate()
        try:
            server.wait(timeout=2)
        except Exception:
            server.kill()
            server.wait()
