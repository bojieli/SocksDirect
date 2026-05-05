# Third-party code

Inventory of code that is not original to SocksDirect, the form in
which it lives in this tree, its license, and how it's invoked. The
naming-consistency CI gate excludes the paths listed here so we don't
churn upstream files; the security-review reads from this list to know
where to look for known-CVE'd dependencies.

## Vendored at source-level

### HERD — `rdma/hrd_*.{cc,h}`

- Upstream: <https://github.com/efficient/HERD>
- License: BSD-3-Clause
- Used by: `libsd` and the RDMA microbenchmarks in `rdma/`,
  `bench/baselines/raw-rdma/`. The HERD helper provides the
  experimental-verbs path that the paper's RDMA results were measured
  on.
- Modifications: minor portability fixes (none security-relevant).
- Build gating: `-DSOCKSDIRECT_WITH_HERD=ON`. Off by default —
  requires Mellanox OFED experimental verbs that aren't in stock
  `rdma-core`.
- Migration plan: HERD is the legacy code path. `src/lib/rdma.cpp`
  (Phase 3) drops the experimental-verbs dependency in favor of
  stock `rdma-core` for the post-rewrite tree.

### googletest — fetched at configure time

- Upstream: <https://github.com/google/googletest>
- License: BSD-3-Clause
- Used by: every `tests/unit/test_*.cpp`.
- Pinned: v1.14.0 with SHA256 verification (see `tests/CMakeLists.txt`).
- Mode: `FetchContent` — only fetched if no system `libgtest-dev`
  is available, so distro packages are preferred.

## Referenced but not yet vendored

### tomlplusplus

- License: MIT
- Status: planned dependency for the production config loader
  (`include/socksdirect/config.hpp` is a stop-gap INI loader). When
  it lands, it'll be added via `FetchContent` like googletest.

### spdlog

- License: MIT
- Status: planned dependency for the production logger
  (`include/socksdirect/log.hpp` is a stop-gap stdlib-only logger).
  Same `FetchContent` pattern.

## Paper-only artifacts

### SocksDirect paper sources

- Upstream: separate `SocksDirect-paper/` repository.
- Status: gitignored from this tree. The rewrite plan has it as a
  submodule under `paper/`; not wired yet — the paper sources are
  not needed by anything in `src/`/`include/`/`tests/`.
- License: as the paper repo states.

### Plot templates

- Upstream: copied from `SocksDirect-paper/eval/`.
- Destination after Phase 6: `reproduce/plot-templates/` (currently
  empty; per-figure scripts will land them).

## How to add a new third-party dependency

1. Add an entry here describing the upstream, license, why we need
   it, and how it's gated.
2. Prefer header-only or `FetchContent` to vendoring; we'd rather
   take a build-time fetch than a copy that drifts.
3. Update `.github/workflows/ci.yml` if the dep needs system
   packages installed in the runner.
4. If the license is anything other than MIT / BSD / Apache-2.0 /
   ISC, raise it in the PR description before writing the
   integration code — copyleft deps need legal review.

## License-compatibility summary

The userspace library and tooling are released under the
project's open-source license (see the project root). HERD's
BSD-3-Clause and googletest's BSD-3-Clause are compatible with
either Apache-2.0 or MIT-style licensing. The kernel module
(`src/kernel/`) is GPL-2.0-licensed per Linux requirements; it
links no userspace code.
