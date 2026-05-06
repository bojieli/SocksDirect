# tier2.pkr.hcl — packer manifest for the Tier-2 (intra-host perf) VM.
#
# Same base as tier1 + hugepages preconfigured + the SocksDirect DKMS
# module preinstalled. CPU pinning is the operator's responsibility at
# `qemu-system-x86_64 -smp ... -cpu host -enable-kvm`.

packer {
  required_plugins {
    qemu = { version = ">= 1.0.10", source = "github.com/hashicorp/qemu" }
  }
}

variable "iso_url"            { type = string  default = "https://releases.ubuntu.com/22.04/ubuntu-22.04.4-live-server-amd64.iso" }
variable "iso_checksum"       { type = string  default = "file:https://releases.ubuntu.com/22.04/SHA256SUMS" }
variable "socksdirect_branch" { type = string  default = "master" }
variable "hugepages"          { type = number  default = 1024 }

source "qemu" "tier2" {
  iso_url          = var.iso_url
  iso_checksum     = var.iso_checksum
  output_directory = "build/tier2"
  vm_name          = "socksdirect-tier2.qcow2"
  format           = "qcow2"
  disk_size        = "16G"
  memory           = 8192
  cpus             = 8
  accelerator      = "kvm"
  headless         = true
  # See tier1.pkr.hcl for the security note on these defaults.
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
  sources = ["source.qemu.tier2"]

  provisioner "shell" {
    inline = [
      "set -e",
      "sudo apt-get update",
      "sudo apt-get install -y cmake build-essential git python3-pytest libgtest-dev googletest libibverbs-dev rdma-core ibverbs-providers iproute2 nginx redis-server redis-tools dkms linux-headers-generic",
      "sudo modprobe rdma_rxe || true",
      "iface=$(ip -o -4 route show to default | awk '{print $5}' | head -n1); sudo rdma link add rxe0 type rxe netdev $iface || true",
      "echo 'vm.nr_hugepages = ${var.hugepages}' | sudo tee /etc/sysctl.d/10-socksdirect-hugepages.conf",
      "sudo sysctl --system",
      "git clone --branch ${var.socksdirect_branch} https://github.com/bojieli/SocksDirect.git /opt/socksdirect",
      "cd /opt/socksdirect && cmake -S . -B build -DSOCKSDIRECT_WITH_RDMA=ON && cmake --build build -j",
      # Register the DKMS module. Build is best-effort; reproduction
      # falls back to copy mode if the module fails to load on this
      # particular kernel.
      "sudo cp -r /opt/socksdirect/src/kernel /usr/src/socksdirect-0.1.0",
      "sudo cp /opt/socksdirect/packaging/dkms/dkms.conf /usr/src/socksdirect-0.1.0/dkms.conf",
      "sudo dkms add socksdirect/0.1.0 || true",
      "sudo dkms build socksdirect/0.1.0 || true",
      "sudo dkms install socksdirect/0.1.0 || true",
      "sudo modprobe socksdirect || true",
      "ls -l /dev/socksdirect 2>/dev/null || echo 'kernel module not loaded; copy-mode fallback'",
    ]
  }
}
