"""Test that the kernel/userspace ABI consistency checker fires when
struct fields drift apart.

The actual gate runs in CI on the real headers; here we verify the
tool itself doesn't have false positives or false negatives.
"""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TOOL = REPO / "tools" / "check_kernel_abi.py"
USER = REPO / "include" / "socksdirect" / "zerocopy.h"
KERN = REPO / "src" / "kernel" / "socksdirect_dev.h"


def test_real_headers_are_in_sync():
    r = subprocess.run(["python3", str(TOOL)], stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10)
    assert r.returncode == 0, r.stdout + r.stderr


def test_drift_is_detected(tmp_path):
    # Stage a copy of the tool with redirected paths.
    work_repo = tmp_path / "repo"
    work_repo.mkdir()
    (work_repo / "include" / "socksdirect").mkdir(parents=True)
    (work_repo / "src" / "kernel").mkdir(parents=True)
    (work_repo / "tools").mkdir()
    shutil.copy(USER, work_repo / "include" / "socksdirect" / "zerocopy.h")
    shutil.copy(KERN, work_repo / "src" / "kernel" / "socksdirect_dev.h")
    shutil.copy(TOOL, work_repo / "tools" / "check_kernel_abi.py")

    # Mutate the kernel-side header to remove a field.
    kt = (work_repo / "src" / "kernel" / "socksdirect_dev.h").read_text()
    kt2 = kt.replace("__u64 cookie;\n", "// cookie removed\n", 1)
    assert kt != kt2, "expected at least one cookie field to drop"
    (work_repo / "src" / "kernel" / "socksdirect_dev.h").write_text(kt2)

    r = subprocess.run(
        ["python3", str(work_repo / "tools" / "check_kernel_abi.py")],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=10,
    )
    assert r.returncode == 1
    assert b"mismatch" in r.stdout or b"drift" in r.stdout
