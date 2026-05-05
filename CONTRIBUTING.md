# Contributing to SocksDirect

Thanks for considering a contribution. The code is being rewritten from
a research prototype to a production-ready system; the overall plan is
in `REWRITE_PLAN.md` at the root of the repo. Please skim the section
relevant to your change before opening a PR.

## Quick start

```bash
# Build + run unit tests on stock Ubuntu (no RDMA needed):
cmake -S . -B build -DSOCKSDIRECT_WITH_RDMA=OFF
cmake --build build -j
ctest --test-dir build --output-on-failure
```

You don't need a Mellanox NIC, root, or a custom kernel for the unit
suite — those are reserved for Tier 2 / Tier 3 reproduction (see
`docs/REPRODUCIBILITY.md`).

## What we accept

- **Bug fixes** with a regression test that fails before the fix.
- **API completeness** items from `docs/API.md` (the items marked
  `UNSUPPORTED` in particular).
- **Reproduction polish**: new figures under `reproduce/figures/`,
  inventory.yml schema improvements, plot-template fixes.
- **Docs**: examples, troubleshooting, configuration knob descriptions.

## What we'd push back on

- Performance optimizations without a benchmark in
  `bench/microbench/` showing the win.
- New transports. RDMA stays as the inter-host transport.
- Refactors that span more than ~3 files without a PR description
  explaining the unifying motivation.
- Changes to the public API surface in `include/socksdirect/` without
  a CHANGELOG entry and an ABI-compat note.

## Running the full test matrix

```bash
# Unit tests (always)
ctest --test-dir build -R '^test_'

# Integration tests (skip if libsd / monitor / ctl absent)
ctest --test-dir build -R '^integration-'

# Sanitizers
cmake -S . -B build-asan \
      -DCMAKE_BUILD_TYPE=Debug \
      -DSOCKSDIRECT_WITH_ASAN=ON \
      -DSOCKSDIRECT_WITH_UBSAN=ON
cmake --build build-asan -j
ctest --test-dir build-asan
```

CI (`.github/workflows/ci.yml`) runs the same matrix on Ubuntu 22.04
and 24.04, plus a docs-lint and naming-consistency gate. PRs are
expected to be green.

## Style

- C/C++: follow the surrounding code; keep new headers in
  `include/socksdirect/` header-only and stdlib-only unless you have
  a strong reason. We are intentionally dependency-free until the
  first heavy dep ships.
- Python: idiomatic Python 3.10+; type hints on new code; no third-party
  deps in `tools/` or `reproduce/repro` so they run on a stock VM.
- Tests: every new public-API entry point gets a unit test covering
  happy path, error path, and at least one edge case. Performance
  changes get a microbenchmark.

## Commit messages

- One concern per commit; revertable in isolation.
- The first line is imperative and ≤72 chars.
- Body explains *why*, not what — git diff already shows the latter.
- Reference rewrite plan phases when relevant: `Phase 4: ...`.

## Reporting issues

Include: kernel version (`uname -a`), distro, whether the LKM is loaded
(`lsmod | grep socksdirect`), the failing command, and the relevant
section of `/var/log/socksdirect/<pid>.log`. For perf reports, include
the output of `socksdirect-ctl status` and the bench JSON.

## License

By contributing, you agree your contribution is licensed under the
project's open-source license (see the project root).
