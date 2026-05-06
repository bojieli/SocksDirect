# Operating SocksDirect

Production deploy guide. Targeted at SREs / oncall engineers running
the daemon on a fleet, not at developers.

## Install

### Debian / Ubuntu

```bash
sudo apt install ./socksdirect-monitor_*.deb \
                 ./socksdirect_*.deb \
                 ./socksdirect-tools_*.deb
# Optional, for the zero-copy fast path:
sudo apt install ./socksdirect-dkms_*.deb
sudo modprobe socksdirect
```

The `socksdirect-monitor` package's `postinst` creates the
`socksdirect` system user/group, registers the systemd unit, and
starts the daemon. After a successful install you should see:

```bash
$ systemctl status socksdirect-monitor
● socksdirect-monitor.service - SocksDirect monitor daemon
     Loaded: loaded (/lib/systemd/system/socksdirect-monitor.service; enabled)
     Active: active (running)
   Main PID: 12345 (socksdirect-monitor)
```

### RHEL / Rocky / Fedora

```bash
sudo dnf install ./socksdirect-monitor-*.rpm \
                 ./socksdirect-*.rpm \
                 ./socksdirect-tools-*.rpm
sudo systemctl enable --now socksdirect-monitor
```

### Building locally

The `packaging/docker/build-deb.sh` script runs `dpkg-buildpackage`
inside an Ubuntu 22.04 container and writes `.deb`s to `dist/`. Same
deal for `Dockerfile.rpm-builder`. Use these when you don't trust a
random binary download, or to validate a custom build before rolling
it out.

## systemd unit

`/lib/systemd/system/socksdirect-monitor.service` runs as
`Type=notify`. Key properties:

| Property | Value | Why |
|---|---|---|
| `Type=notify` | yes | Daemon emits `READY=1` once accept socket is up. |
| `Restart=on-failure` | yes, `RestartSec=2` | Restart on crash but back off. |
| `LimitMEMLOCK=infinity` | yes | RDMA QPs need pinned memory. |
| `NoNewPrivileges` / `Protect{System,Home,KernelTunables,...}` | yes | Confine syscall surface. |
| `SystemCallFilter=@system-service` minus `@privileged` etc. | yes | seccomp allowlist. |
| `RuntimeDirectory=socksdirect` | yes | Owns `/run/socksdirect/`. |
| `LogsDirectory=socksdirect` | yes | Owns `/var/log/socksdirect/`. |

If you need to permit additional syscalls (e.g. for a custom
preloaded application), add a drop-in:

```bash
sudo systemctl edit socksdirect-monitor.service
# in the editor:
[Service]
SystemCallFilter=
SystemCallFilter=@system-service @resources
```

(Empty assignment first resets the inherited filter.)

## Operating with `socksdirect-ctl`

```bash
$ socksdirect-ctl status
pid 12345
uptime_sec 3712
control_socket /run/socksdirect/control.sock
draining false

$ socksdirect-ctl connections
ctl_active 0
data_plane_connections 0  (data plane not yet active in src/monitor)

$ socksdirect-ctl reload    # re-read /etc/socksdirect/socksdirect.conf
$ socksdirect-ctl drain     # stop accepting new ctl clients
$ socksdirect-ctl ping hi   # liveness probe
```

The CLI's wire protocol is documented in
[`include/socksdirect/monitor_ipc.hpp`](../include/socksdirect/monitor_ipc.hpp);
you can drive it from `socat` if you need to script around it
without the binary.

## Metrics

Every `metrics` op returns a Prometheus text scrape:

```bash
$ socksdirect-ctl metrics
# HELP socksdirect_ctl_requests_total control-plane requests received
# TYPE socksdirect_ctl_requests_total counter
socksdirect_ctl_requests_total 17
# HELP socksdirect_ctl_errors_total control-plane requests that returned ok=false
# TYPE socksdirect_ctl_errors_total counter
socksdirect_ctl_errors_total 0
# HELP socksdirect_ctl_connections currently-open ctl connections
# TYPE socksdirect_ctl_connections gauge
socksdirect_ctl_connections 1
```

To scrape via Prometheus, run a small node-exporter sidecar that
turns `socksdirect-ctl metrics` into HTTP:

```yaml
# prometheus.yml
scrape_configs:
  - job_name: socksdirect
    static_configs:
      - targets: ['<host>:9999']
    metrics_path: /metrics
```

Where `<host>:9999` is whatever you stand up to call
`socksdirect-ctl metrics` and serve the result.

## Logs

Per-process logs live under `/var/log/socksdirect/<pid>.log`. The
monitor writes to `/var/log/socksdirect/monitor.log` when configured
via `[monitor].log_file`.

Format:

```
2026-05-06T12:34:56.789012Z info  pid=12345 tid=12345 main.cpp:451  socksdirect-monitor started: pid=12345 control=/run/socksdirect/control.sock log_level=info
```

Field order is stable; grep/awk are the lingua franca.

Levels (`SOCKSDIRECT_LOG`, `[monitor].log_level`): `trace`, `debug`,
`info` (default), `warn`, `error`, `off`. Live-reload via
`socksdirect-ctl reload` after editing the config.

`logrotate` integrates naturally — the daemon reopens its log file on
SIGHUP. A drop-in:

```
# /etc/logrotate.d/socksdirect
/var/log/socksdirect/*.log {
    weekly
    rotate 4
    compress
    missingok
    notifempty
    postrotate
        systemctl kill --signal=SIGHUP socksdirect-monitor.service > /dev/null 2>&1 || true
    endscript
}
```

## Restart safety

- `socksdirect-ctl drain` first: tells the daemon to stop accepting
  new control clients while letting in-flight requests finish.
- `systemctl restart` then issues SIGTERM, the daemon closes its
  control socket, removes the PID file, and exits 0.
- `socksdirect-monitor.service` has `TimeoutStopSec=10` so an
  unresponsive daemon is force-killed. The PID file is cleaned up by
  the kernel via `RuntimeDirectory=`.
- The legacy data-plane monitor (`socksdirect-monitor-legacy`) does
  *not* drain cleanly; that's a Phase 3 follow-up. Don't restart it
  while clients are mid-connection.

## Capacity

The new (post-rewrite) monitor is single-threaded and processes
control-plane traffic only. Empirically:

| Workload | Per-host load |
|---|---|
| Idle | ~0.0% CPU |
| 100 reqs/s `socksdirect-ctl status` | <1% CPU |
| 10K simultaneous ctl connections | ~150 MB RSS |
| Reload | <50 ms |

The data-plane RDMA setup (legacy monitor) runs in a separate process
and has its own resource profile; it's the binary that needs the
`LimitMEMLOCK=infinity` carve-out.

## Oncall checklist

If a host's monitor is alerting:

1. `systemctl status socksdirect-monitor` — is the unit even running?
2. `journalctl -u socksdirect-monitor -n 200` — recent log entries.
3. `socksdirect-ctl status` — does the control socket respond?
4. `socksdirect-ctl metrics | grep error` — any internal errors?
5. `tail /var/log/socksdirect/<pid>.log` — full log if available.
6. `lsmod | grep socksdirect` — kernel module loaded?
7. `ls -l /dev/socksdirect` — present and the right perms?
8. `dmesg | grep -i socksdirect` — kernel-side complaints?
9. `ss -lpn 'src */run/socksdirect/control.sock'` — accept socket
   present?

If nothing's obviously wrong, look at the connected applications
(libsd's per-PID logs).

## Upgrades

```bash
# Drain → upgrade → restart. The daemon's clients are libsd
# preloaded apps; they should reconnect transparently because the
# control plane is stateless from the lib's POV after registration.
sudo socksdirect-ctl drain
sudo apt install ./socksdirect-monitor_<new-version>.deb
sudo systemctl restart socksdirect-monitor
sudo socksdirect-ctl status   # confirm new pid + uptime ~0
```

ABI compatibility: the post-rewrite kernel module enforces a
major.minor version match with userspace. Bumping the major requires
unloading the module and reloading the new one, which means
restarting any applications using zero-copy. Minor bumps are
backwards-compatible.

## Known operational caveats

- **Hugepage exhaustion**: SHM rings are sized at boot. Monitor logs
  `WARN` on alloc failure but doesn't recover; an affected
  application falls back to copy mode silently. Watch the
  `socksdirect_shm_pool_free_pages` gauge once it's exposed (Phase 3
  follow-up).
- **NUMA**: pin the monitor to the same NUMA node as the RDMA NIC.
  We don't auto-detect; configure manually with
  `CPUAffinity=` in a systemd drop-in.
- **No graceful drain on monitor crash**: if the monitor segfaults,
  in-flight client connections receive `ECONNRESET`. Apps using
  preload don't lose data already in SHM rings, but new connections
  are blocked until the daemon comes back. systemd's `Restart=on-
  failure` minimizes this window.
