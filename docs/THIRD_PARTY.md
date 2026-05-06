# Third-party code

Inventory of code that is not original to SocksDirect, the form in
which it lives in this tree, its license, and how it's invoked. The
naming-consistency CI gate excludes the paths listed here so we don't
churn upstream files; the security-review reads from this list to know
where to look for known-CVE'd dependencies.

## Vendored at source-level

### HERD-derived helpers — `rdma/hrd_*.{cc,h}`

- Upstream: the C++ `hrd.{h,cc}` files match the
  `libhrd_cpp/` directory of <https://github.com/efficient/rdma_bench>
  (a successor of the original HERD project at
  <https://github.com/efficient/HERD>). Authors: Anuj Kalia,
  Michael Kaminsky, David G. Andersen (Carnegie Mellon).
- **License: NOT DECLARED upstream as of 2026-05-06.** Both
  `efficient/HERD` and `efficient/rdma_bench` ship without a
  LICENSE file; GitHub's API reports `license: null` for both.
  By default that means "all rights reserved": there is no
  blanket permission to redistribute under Apache-2.0.
- **Implication for SocksDirect's Apache-2.0 release:** the source
  tarball / GitHub Release artifacts produced by
  `.github/workflows/release.yml` exclude these files unless the
  builder is operating under an out-of-band redistribution grant
  from the HERD authors. The default build (`-DSOCKSDIRECT_WITH_HERD=OFF`)
  doesn't compile them; the `-DSOCKSDIRECT_WITH_HERD=ON` opt-in
  is intended for the paper-reproduction path on a developer
  machine, not for distributable binaries.
- Used by: `sd-legacy.so` (the RDMA + HERD opt-in build) and the
  RDMA microbenchmarks in `rdma/`, `bench/baselines/raw-rdma/`.
- Migration plan: replace with the equivalent functionality from
  <https://github.com/erpc-io/eRPC> (Apache-2.0, same authors) or
  rewrite against stock `rdma-core` in Phase 3. Tracked in
  `docs/MISSING_FEATURES.md`.

If you want to consume the HERD path:
1. Verify your situation against the upstream license status (it
   may have changed since this doc was written).
2. Or obtain written permission from the HERD authors.
3. Do not distribute binaries linked against `libhrd.so` from a
   source tarball that doesn't include explicit grant documentation.

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
