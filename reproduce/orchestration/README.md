# reproduce/orchestration

Two-host orchestration via Ansible — replaces ad-hoc `ssh ... &`
calls embedded in the legacy figure scripts.

## Quick start

```bash
# 1. Render the harness inventory as an Ansible inventory.
python3 tools/inv-to-ansible.py reproduce/inventory.yml \
    > reproduce/orchestration/inventory.ini

# 2. Run a figure across both hosts.
ansible-playbook \
    -i reproduce/orchestration/inventory.ini \
    reproduce/orchestration/playbook.yml \
    -e figure=msgsize-inter
```

The playbook installs build dependencies, clones the repo at the
requested branch, builds libsd, then runs the figure on the driver
host. Peer-side processes are launched inline by the figure's `run.sh`
via SSH.

## What this exists to fix

The legacy `data/*/test_*.sh` scripts hardcoded `ssh user@10.1.2.34 ...`
all over the place. Each environment had to fork the scripts. The
playbook keeps the inventory in one file and the SSH plumbing in
Ansible.

## When you don't need this

For Tier-1 figures (`queue-microbench`, `control-plane-overhead`,
`loopback-baseline`, ...) just run `./reproduce/repro <name>` directly
— they're single-host and don't touch SSH.
