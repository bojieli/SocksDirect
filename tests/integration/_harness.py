"""Helpers for spawning processes under LD_PRELOAD=libsd.so.

Kept separate from conftest.py so it can be imported by individual tests
without the test discovery side effects of conftest.
"""

from __future__ import annotations

import os
import subprocess
from pathlib import Path


def preloaded(libsd_path: Path, argv: list[str], **popen_kwargs) -> subprocess.Popen:
    """Spawn `argv` with libsd preloaded.

    Caller is responsible for waiting on or terminating the returned
    process. Stdout/stderr default to PIPE so callers can assert.
    """
    env = os.environ.copy()
    env["LD_PRELOAD"] = str(libsd_path)
    env.setdefault("SOCKSDIRECT_LOG", "warn")
    return subprocess.Popen(
        argv,
        env=env,
        stdout=popen_kwargs.pop("stdout", subprocess.PIPE),
        stderr=popen_kwargs.pop("stderr", subprocess.PIPE),
        **popen_kwargs,
    )


def run_preloaded(libsd_path: Path, argv: list[str], timeout: float = 10.0) -> subprocess.CompletedProcess:
    """Run `argv` to completion under libsd preload, returning the result.

    Times out after `timeout` seconds and raises subprocess.TimeoutExpired.
    """
    env = os.environ.copy()
    env["LD_PRELOAD"] = str(libsd_path)
    env.setdefault("SOCKSDIRECT_LOG", "warn")
    return subprocess.run(
        argv,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
        check=False,
    )
