# SocksDirect documentation

Start here. This page is a map: it tells you what to read in what
order, depending on why you arrived.

## "I want to try it on my laptop"

1. [`README`](../README.md) — the 5-minute quick start.
2. [`docs/TROUBLESHOOTING`](TROUBLESHOOTING.md) when something goes
   sideways.
3. The `apps/` directory has self-contained demos (nginx, redis,
   rpclib, an NFV pipeline).

## "I want to reproduce the paper"

1. [`docs/REPRODUCIBILITY`](REPRODUCIBILITY.md) — three reproduction
   tiers, what each can and cannot prove, per-figure expectations.
2. [`reproduce/inventory.example.yml`](../reproduce/inventory.example.yml)
   — the single config file the harness reads.
3. [`./reproduce/repro check`](../reproduce/repro) auto-detects
   your tier; `./reproduce/repro all` runs every figure for it.
4. For two-host runs:
   [`reproduce/orchestration/`](../reproduce/orchestration/README.md).
5. For pre-built VM images: [`reproduce/vm-images/`](../reproduce/vm-images/README.md).

## "I want to deploy SocksDirect to my hosts"

1. [`docs/OPERATIONS`](OPERATIONS.md) — install, systemd, metrics,
   oncall checklist.
2. [`docs/CONFIGURATION`](CONFIGURATION.md) — every config knob with
   defaults and env-var overrides.
3. [`docs/SECURITY`](SECURITY.md) — trust model and what isn't
   defended.
4. [`docs/KERNEL_MODULE`](KERNEL_MODULE.md) — the optional zero-copy
   LKM; how to install via DKMS; copy-mode fallback if you don't.

## "I want to embed / extend SocksDirect"

1. [`docs/ARCHITECTURE`](ARCHITECTURE.md) — paper sections → code
   modules. The dependency rules.
2. [`docs/API`](API.md) — every libc function libsd hooks, with
   support level. Auto-generated from
   [`tests/conformance/coverage.toml`](../tests/conformance/coverage.toml).
3. [`include/socksdirect/`](../include/socksdirect/) — the public,
   header-only APIs (`Config`, `Logger`, `Metrics`, `MonitorIpc`,
   `FdRemapTable`, `ZeroCopyClient`).
4. [`CONTRIBUTING`](../CONTRIBUTING.md) — what we accept; how to run
   the test matrix.

## "I'm trying to understand what's still missing"

1. [`REWRITE_PLAN`](../REWRITE_PLAN.md) — the multi-phase plan with
   exit criteria.
2. [`docs/MISSING_FEATURES`](MISSING_FEATURES.md) — every paper-
   described feature that isn't yet implemented.
3. [`docs/PERFORMANCE`](PERFORMANCE.md) — what's actually fast today
   vs. what's planned.
4. [`docs/error-handling-audit`](error-handling-audit.md) — every
   `FATAL`/`assert` site in the legacy tree, with disposition.
5. [`docs/MIGRATION`](MIGRATION.md) — what changed if you're coming
   from the prototype.
6. [`docs/THIRD_PARTY`](THIRD_PARTY.md) — vendored / fetched
   dependencies, their licenses, and which build flags pull them in.

## "I have a question that doesn't fit any of these"

[`docs/FAQ`](FAQ.md) collects the recurring ones.

## Quick reference

```
include/socksdirect/    Public, header-only APIs.
src/lib/                libsd preload library.
src/monitor/            socksdirect-monitor daemon.
src/kernel/             /dev/socksdirect LKM.
tools/                  socksdirect-ctl + scripts.
bench/microbench/       Standalone microbenchmarks.
tests/unit/             gtest unit tests.
tests/integration/      pytest integration tests.
tests/conformance/      libc-API conformance suite.
reproduce/              Paper-figure reproduction harness.
apps/                   Demo / reproduction binaries.
packaging/              .deb, .rpm, DKMS, systemd.

common/, lib/, monitor/ Legacy research-prototype trees. Phase 1+ of
                        the rewrite migrates these into src/. Don't
                        add new code here.
```

## Doc completeness gates

CI fails on drift between:
- `docs/API.md` and `tests/conformance/coverage.toml`
  (`tools/render_api_doc.py --check`).
- `docs/error-handling-audit.md` and the legacy tree
  (`tools/scan_error_handling.py --check`).
- `include/socksdirect/zerocopy.h` and `src/kernel/socksdirect_dev.h`
  (`tools/check_kernel_abi.py`).

The naming-consistency gate prevents pre-rewrite identifiers from
slipping back into the post-rewrite tree.
