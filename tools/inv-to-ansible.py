#!/usr/bin/env python3
"""inv-to-ansible.py — render reproduce/inventory.yml as an Ansible
inventory.ini.

Reads our YAML inventory (the one the harness already speaks) and
emits something `ansible-playbook -i ...` accepts. The first listed
host becomes the `driver` group; the rest become `peer`. All hosts
are also in `all`.

Usage:
    inv-to-ansible.py reproduce/inventory.yml > inventory.ini
"""
from __future__ import annotations
import argparse, sys
from pathlib import Path

# Reuse the harness's tiny YAML loader so we depend on nothing.
import importlib.machinery as _mach
import importlib.util as _u
_repro_path = Path(__file__).resolve().parents[1] / "reproduce" / "repro"
_loader = _mach.SourceFileLoader("repro", str(_repro_path))
_spec = _u.spec_from_loader("repro", _loader)
_m = _u.module_from_spec(_spec)
_loader.exec_module(_m)


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("inventory", type=Path)
    p.add_argument("--remote-repo-path", default="/opt/socksdirect")
    args = p.parse_args()
    inv = _m.load_inventory(args.inventory)
    hosts = inv.get("hosts") or {}
    if not hosts:
        print("error: inventory.yml has no hosts", file=sys.stderr)
        return 2
    items = list(hosts.items())
    driver_name, _ = items[0]
    print("[all:vars]")
    print(f"remote_repo_path = {args.remote_repo_path}")
    print()
    print("[driver]")
    for name, info in items[:1]:
        ssh = info.get("ssh", name)
        rip = info.get("rdma_ip", "")
        nic = info.get("nic", "mlx5_0")
        print(f"{name} ansible_host={ssh} rdma_ip={rip} nic={nic}")
    print()
    print("[peer]")
    for name, info in items[1:]:
        ssh = info.get("ssh", name)
        rip = info.get("rdma_ip", "")
        nic = info.get("nic", "mlx5_0")
        print(f"{name} ansible_host={ssh} rdma_ip={rip} nic={nic}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
