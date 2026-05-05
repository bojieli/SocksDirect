"""Preloading libsd into trivial commands must not break them.

This is the most basic compatibility gate: `LD_PRELOAD=libsd.so /bin/true`
must exit 0. If the constructor crashes or the monitor handshake fails,
every other test goes red — diagnose it here first.
"""

from __future__ import annotations

import os
from pathlib import Path

import pytest

from _harness import run_preloaded


def test_preload_into_true(libsd_path: Path, monitor):
    """`LD_PRELOAD=libsd.so /bin/true` must succeed."""
    r = run_preloaded(libsd_path, ["/bin/true"], timeout=5)
    if r.returncode != 0:
        pytest.fail(
            f"preloaded /bin/true exited {r.returncode}\n"
            f"stdout: {r.stdout.decode(errors='replace')}\n"
            f"stderr: {r.stderr.decode(errors='replace')}"
        )


def test_preload_into_echo(libsd_path: Path, monitor):
    """`echo hello` under preload must print 'hello'."""
    r = run_preloaded(libsd_path, ["/bin/echo", "hello"], timeout=5)
    assert r.returncode == 0, r.stderr.decode(errors="replace")
    assert r.stdout.strip() == b"hello"


def test_preload_into_ls(libsd_path: Path, monitor):
    """`ls /tmp` under preload must succeed (exercises file-fd path)."""
    r = run_preloaded(libsd_path, ["/bin/ls", "/tmp"], timeout=5)
    assert r.returncode == 0, r.stderr.decode(errors="replace")
