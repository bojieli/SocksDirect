# tier1.pkr.hcl — packer manifest for the Tier-1 (functional) VM image.
#
# Stock Ubuntu 22.04 + SocksDirect packages + SoftRoCE configured so
# inter-host figures *run* end-to-end (with the report banner that
# excludes their numbers from comparison).
#
# Build:
#   packer init reproduce/vm-images
#   packer build reproduce/vm-images/tier1.pkr.hcl
#
# Output: socksdirect-tier1.qcow2 (~2 GB).
#
# This manifest produces an image that boots straight into a working
# `./reproduce/repro check && ./reproduce/repro all` environment.

packer {
  required_plugins {
    qemu = {
      version = ">= 1.0.10"
      source  = "github.com/hashicorp/qemu"
    }
  }
}

variable "iso_url" {
  type    = string
  default = "https://releases.ubuntu.com/22.04/ubuntu-22.04.4-live-server-amd64.iso"
}

variable "iso_checksum" {
  type    = string
  default = "file:https://releases.ubuntu.com/22.04/SHA256SUMS"
}

variable "socksdirect_branch" {
  type    = string
  default = "master"
}

source "qemu" "tier1" {
  iso_url          = var.iso_url
  iso_checksum     = var.iso_checksum
  output_directory = "build/tier1"
  vm_name          = "socksdirect-tier1.qcow2"
  format           = "qcow2"
  disk_size        = "8G"
  memory           = 2048
  cpus             = 2
  accelerator      = "kvm"
  headless         = true
  ssh_username     = "ubuntu"
  ssh_password     = "ubuntu"
  ssh_timeout      = "30m"
  shutdown_command = "sudo shutdown -P now"
  http_directory   = "${path.root}/http"
  boot_command = [
    "<wait>",
    "c<wait>",
    "linux /casper/vmlinuz quiet autoinstall ds=nocloud-net\\;s=http://{{.HTTPIP}}:{{.HTTPPort}}/ ---<enter>",
    "initrd /casper/initrd<enter>",
    "boot<enter>",
  ]
}

build {
  sources = ["source.qemu.tier1"]

  provisioner "shell" {
    inline = [
      "set -e",
      "sudo apt-get update",
      "sudo apt-get install -y cmake build-essential git python3-pytest libgtest-dev googletest libibverbs-dev rdma-core ibverbs-providers iproute2 nginx redis-server redis-tools",
      "sudo modprobe rdma_rxe || true",
      # Configure SoftRoCE on the primary interface so inter-host figures
      # at least *run* (their results are excluded from the report; see
      # docs/REPRODUCIBILITY.md).
      "iface=$(ip -o -4 route show to default | awk '{print $5}' | head -n1); sudo rdma link add rxe0 type rxe netdev $iface || true",
      "git clone --branch ${var.socksdirect_branch} https://github.com/bojieli/SocksDirect.git /opt/socksdirect",
      "cd /opt/socksdirect && cmake -S . -B build -DSOCKSDIRECT_WITH_RDMA=ON && cmake --build build -j",
      "echo 'cd /opt/socksdirect && ./reproduce/repro check' | sudo tee /etc/profile.d/socksdirect-banner.sh",
    ]
  }
}
