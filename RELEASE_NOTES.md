# Release notes

## v0.1.0-preview — 2026-05-06

First public release of the post-rewrite SocksDirect tree. Marked
**preview** rather than `1.0` because the SHM data plane port from
the legacy library to `src/lib/` (the work that delivers the paper's
intra-host perf claims) is the remaining engineering item; see
`docs/PERFORMANCE.md` for the honest picture.

### What's in the release

- `src/lib/libsd.so` — new preload library. Correct semantics for
  every libc function in `tests/conformance/coverage.toml` (24 entries,
  exercised under preload by `integration-libsd-preload`).
  **Instrumented passthrough today**; the SHM data plane lands in a
  follow-up.
- `src/monitor/socksdirect-monitor` — production daemon. 9 control
  ops, sd_notify(READY=1), graceful drain on SIGTERM,
  Prometheus-text metrics. End-to-end test suite
  (`integration-monitor-daemon`).
- `tools/socksdirect-ctl` — control CLI.
- `src/kernel/` — out-of-tree LKM with real page-mapping
  (`get_user_pages_remote` + `vm_insert_page`). DKMS-installable.
  ABI-pinned to userspace via `tools/check_kernel_abi.py`.
- `reproduce/` — 12-figure reproduction harness with three tiers
  (functional VM / intra-host perf / full RDMA), Ansible playbook
  for two-host runs, Packer manifests for tier1/tier2 VM images.
- `apps/` — nginx-demo, redis-demo, rpclib-demo, NFV pipeline.
- `packaging/` — Debian + RPM + DKMS + systemd (with seccomp +
  Type=notify), Docker images for local artifact builds.
- 25 ctest entries pass under both Release and Debug+ASan+UBSan.

### Compatibility caveats

- The new `libsd.so` is correct but doesn't yet beat vanilla Linux
  for intra-host TCP. For the paper's perf claims, build with
  `-DSOCKSDIRECT_WITH_HERD=ON` and use `libsd-legacy.so` (subject
  to the HERD redistribution caveat in `docs/THIRD_PARTY.md`).
- The LKM's page-mapping ioctl path compiles and matches the
  userspace ABI; runtime testing requires loading on bare metal,
  which CI doesn't do.
- Tier-3 (real Mellanox) reproduction figures have correct
  scripts but no actual measurement run included with this
  release.
- HERD-derived helpers (`rdma/hrd_*`) carry no upstream license;
  they're excluded from the source tarball and `.deb`/`.rpm`
  unless the builder opts in. See `NOTICE` and
  `docs/THIRD_PARTY.md`.

### Tagging the release

The `release.yml` workflow runs on `v[0-9]+.[0-9]+.[0-9]+`-shaped
tag pushes. To cut this preview release:

```bash
git tag -a v0.1.0-preview -m "v0.1.0-preview — first post-rewrite release"
git push origin v0.1.0-preview
```

Then check the resulting GitHub Release for the source tarball,
.deb, .rpm, and SHA256SUMS. Don't tag until you've reviewed the
above caveats.

### What lands in v0.2.0 (planned)

- Phase 3 SHM data-plane port from the legacy `lib/socket_lib.cpp`
  to `src/lib/`. Once this lands, `libsd.so` actually delivers
  intra-host speedup and `libsd-legacy.so` can be dropped.
- ~115 FATAL/assert sites in the legacy tree converted to errno
  returns (per `docs/error-handling-audit.md`).
- LKM page-mapping verified on bare metal with a documented test
  matrix.

### What lands in v1.0.0 (planned)

- Tier-3 reproduction run on real Mellanox hardware with
  measurements committed to `reproduce/results/v1-bare-metal/`.
- Self-hosted perf-regression runner deployed and `vars.PERF_RUNNER_AVAILABLE=true`.
- Legacy `lib/`, `monitor/`, `common/` trees removed; only the
  post-rewrite `src/` tree ships.
