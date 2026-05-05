"""Lint the packaging metadata so a syntax slip is caught in CI."""
from __future__ import annotations
import os
import shutil
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]


def test_debian_control_parses_with_dpkg_parsechangelog():
    if not shutil.which("dpkg-parsechangelog"):
        pytest.skip("dpkg-parsechangelog not installed")
    cl = REPO / "packaging" / "debian" / "changelog"
    r = subprocess.run(
        ["dpkg-parsechangelog", "-l", str(cl)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5,
    )
    assert r.returncode == 0, r.stderr


def test_debian_install_files_reference_existing_paths():
    """Each `debian/<pkg>.install` line should reference a path that
    either exists in the source tree or is produced under debian/tmp/
    (the debhelper staging dir)."""
    pkg_dir = REPO / "packaging" / "debian"
    for f in pkg_dir.glob("*.install"):
        for line in f.read_text().splitlines():
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            src = line.split()[0]
            if src.startswith("debian/tmp/"):
                continue  # produced by the build
            full = REPO / src
            # globs like include/socksdirect/*.hpp
            if "*" in src:
                matched = list(REPO.glob(src))
                assert matched, f"{f.name}: glob '{src}' matched nothing"
            else:
                assert full.exists(), f"{f.name}: path '{src}' does not exist"


def test_rpm_spec_files_reference_packaging_paths():
    spec = REPO / "packaging" / "rpm" / "socksdirect.spec"
    body = spec.read_text()
    assert "Name:" in body
    assert "Source0:" in body
    assert "%files" in body
    # All install -m references should point at files that exist.
    for line in body.splitlines():
        if line.lstrip().startswith("install ") and "packaging/" in line:
            for tok in line.split():
                if tok.startswith("packaging/"):
                    assert (REPO / tok).exists(), f"spec references missing {tok}"


def test_systemd_unit_uses_correct_binary_path():
    body = (REPO / "packaging" / "systemd" / "socksdirect-monitor.service").read_text()
    assert "ExecStart=/usr/sbin/socksdirect-monitor" in body
    assert "Type=notify" in body


def test_dkms_conf_present():
    f = REPO / "packaging" / "dkms" / "dkms.conf"
    assert f.exists()
    body = f.read_text()
    assert "PACKAGE_NAME" in body
    assert "BUILT_MODULE_NAME" in body


def test_packer_manifests_exist_and_parse():
    """Sanity: validate the HCL structure of the packer manifests
    without invoking packer (which requires KVM)."""
    for tier in ("tier1", "tier2"):
        f = REPO / "reproduce" / "vm-images" / f"{tier}.pkr.hcl"
        assert f.exists()
        body = f.read_text()
        assert "source \"qemu\"" in body
        assert "build {" in body


def test_ansible_playbook_yaml_parses():
    try:
        import yaml
    except ImportError:
        pytest.skip("PyYAML not installed")
    plays = list(yaml.safe_load_all((REPO / "reproduce" / "orchestration" / "playbook.yml").read_text()))
    plays = [p for p in plays if p]
    assert plays, "playbook empty"
    for play in plays:
        # Ansible plays accept either a list of plays or a single dict.
        if isinstance(play, list):
            for p in play:
                assert "tasks" in p or "hosts" in p
        else:
            assert "hosts" in play
