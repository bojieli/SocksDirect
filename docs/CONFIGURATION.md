# Configuration

SocksDirect reads its configuration from
`/etc/socksdirect/socksdirect.conf` (override via
`SOCKSDIRECT_CONFIG=/path/to/file`). Every value also accepts an
environment-variable override of the form
`SOCKSDIRECT_<SECTION>_<KEY>` (uppercased), so per-process tuning
works without editing the file.

The on-disk format is a minimal INI-with-sections; see
`packaging/socksdirect.conf.example` for a fully commented template.

## Lookup order

For each `(section, key)` pair:

1. `SOCKSDIRECT_<SECTION>_<KEY>` env var (uppercased, `-` and `.`
   become `_`).
2. The corresponding entry in the config file.
3. The default supplied by the caller (documented per-key below).

A missing config file is **not an error** — the loader proceeds with
defaults so dev environments work without `/etc/socksdirect/`.

## Sections

### `[monitor]`

| Key              | Default                                | Notes                                                     |
|------------------|----------------------------------------|-----------------------------------------------------------|
| `socket_path`    | `/run/socksdirect/monitor.sock`        | Where the monitor accepts new clients                     |
| `log_dir`        | `/var/log/socksdirect`                 | Per-process log files land here                           |
| `log_level`      | `info`                                 | `trace`/`debug`/`info`/`warn`/`error`                     |
| `metrics_socket` | `/run/socksdirect/metrics.sock`        | Prometheus text format on a UDS                           |

### `[fd]`

| Key                 | Default     | Notes                                          |
|---------------------|-------------|------------------------------------------------|
| `virtual_fd_base`   | `1073741824` (`0x40000000`) | Below this, kernel owns; above, libsd does |
| `virtual_fd_top`    | `2147483647` (`0x7fffffff`) | Cap on virtual fd allocation              |

### `[shm]`

| Key                | Default | Notes                                       |
|--------------------|---------|---------------------------------------------|
| `hugepage_size_kb` | `2048`  | Must match the kernel's hugepage size       |
| `ring_pages`       | `64`    | Hugepages per ring (≈ ring buffer capacity) |

### `[rdma]`

| Key                 | Default | Notes                                                   |
|---------------------|---------|---------------------------------------------------------|
| `device`            | `auto`  | Mellanox device name; `auto` picks first active port    |
| `gid_index`         | `3`     | RoCEv2 GID; check with `show_gids` from rdma-core       |
| `mtu`               | `4096`  | IB MTU; max on ConnectX-4+                              |
| `qp_depth`          | `128`   | QP send queue depth                                     |
| `batch_completions` | `64`    | CQ poll batch size                                      |
| `fallback`          | `rxe`   | `rxe` = SoftRoCE; `none` = disable inter-host           |

### `[zerocopy]`

| Key                | Default | Notes                                          |
|--------------------|---------|------------------------------------------------|
| `enabled`          | `auto`  | Probes `/dev/socksdirect`; set `off` to force memcpy |
| `min_message_size` | `16384` | Smaller messages always copy                   |

## Inspecting the running config

```bash
socksdirect-ctl dump-state
```

asks the monitor over its control socket (default
`/run/socksdirect/control.sock`, override via `SOCKSDIRECT_CTL_SOCKET`
or `--socket`) to dump every key it resolved at startup, with the
source of each value (env var / file / default). The wire protocol is
in `include/socksdirect/monitor_ipc.hpp`; you can drive it directly
from `socat` if you want to script around it.

## Per-process tuning examples

Run nginx with extra logging, larger ring, and a specific RDMA NIC:

```bash
SOCKSDIRECT_MONITOR_LOG_LEVEL=debug \
SOCKSDIRECT_RDMA_DEVICE=mlx5_1 \
SOCKSDIRECT_SHM_RING_PAGES=128 \
LD_PRELOAD=/usr/lib/libsd.so \
nginx -g 'daemon off;'
```

Disable zero-copy entirely (e.g. when the kernel module isn't loaded):

```bash
SOCKSDIRECT_ZEROCOPY_ENABLED=off LD_PRELOAD=/usr/lib/libsd.so my_app
```
