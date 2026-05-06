# reproduce/vm-images

Packer manifests producing the Tier-1 (functional) and Tier-2
(intra-host perf) VM images called for in `REWRITE_PLAN.md` Phase 6.

## Tier 1 — `tier1.pkr.hcl`

Stock Ubuntu 22.04 + SocksDirect packages + SoftRoCE configured. ~2 GB.
Boots into an environment where `./reproduce/repro check && ./reproduce/repro all`
just works. Inter-host figures *run* (transport=rxe), but the report
banner excludes them from comparison.

## Tier 2 — `tier2.pkr.hcl`

As tier1 + hugepages preconfigured + the LKM preinstalled via DKMS.
Reproduces every intra-host figure within ~10% of paper. Inter-host
figures still skipped.

## Build

```bash
cd reproduce/vm-images
packer init .
packer build tier1.pkr.hcl     # ~30 min, needs KVM
packer build tier2.pkr.hcl     # ~45 min
```

Produces `build/tier1/socksdirect-tier1.qcow2` and
`build/tier2/socksdirect-tier2.qcow2`.

## Run

The included `Vagrantfile` brings up two VMs on a private virtio-net
fabric using whichever box you configure (`VAGRANT_BOX` env var or
edit the file). Inside each VM:

```bash
cd /opt/socksdirect
./reproduce/repro check
./reproduce/repro all
```

## Hardening before redistribution

Both manifests provision the build VM with cloud-init credentials
(`ubuntu` / `ubuntu`) so packer can SSH in during the install. The
resulting qcow2 inherits those credentials. **If you redistribute the
image**, before publishing:

```bash
# Boot the image, then inside it:
sudo passwd -l ubuntu                # lock password login
sudo userdel -r ubuntu || true       # or remove the user entirely
sudo rm /etc/sudoers.d/ubuntu        # drop the NOPASSWD line
sudo cloud-init clean --logs --seed  # wipe instance state
sudo shutdown -h now
```

Then `qemu-img convert` the result to a fresh qcow2 to drop sectors
that the package manager left behind.

For Tier-1 images shared internally for reproduction, the defaults
are fine.

## Validate manifest syntax (no build)

```bash
packer validate reproduce/vm-images/tier1.pkr.hcl
```

CI runs this validation; full builds require KVM and aren't on the
hosted runners.
