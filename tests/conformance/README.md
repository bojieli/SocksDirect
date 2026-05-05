# Conformance suite

The conformance suite exercises every BSD socket function that
`docs/API.md` claims to support, under `LD_PRELOAD=libsd.so` (the
ACCELERATED column) or against unmodified glibc (the PASSTHROUGH
column, used as the reference for the ACCELERATED runs).

Two layers:

1. **`coverage.toml`** — single source of truth listing every
   intercepted libc function, its support level
   (`ACCELERATED` / `PASSTHROUGH` / `UNSUPPORTED`), and the
   matching test case file. `docs/API.md` is generated from this
   file by `tools/render_api_doc.py` (Phase 4 follow-up; today the
   doc is hand-maintained and the gate at
   `tests/integration/test_api_doc_consistency.py` flags drift).

2. **`run_conformance.py`** — driver that walks `coverage.toml`
   and runs the matching test case. Each case is either a small
   C program in `tests/conformance/cases/` or a reference to an
   LTP test under `/opt/ltp/testcases/kernel/syscalls/`.

The CI runs the suite with libsd absent (validates `coverage.toml`
itself plus the PASSTHROUGH expectations). When `SOCKSDIRECT_LIB`
points at a built `libsd.so`, the same driver runs every case
under preload and reports per-case results. PASSTHROUGH tests must
match the no-preload result; ACCELERATED tests must additionally
not crash, hang, or return incorrect data.

## How to run

```bash
# Validate coverage.toml and run the cases that don't need libsd.
ctest --test-dir build -R integration-conformance --output-on-failure

# Run with libsd preloaded (Phase 3+ — when libsd builds without
# RDMA/HERD we'll wire this into CI).
SOCKSDIRECT_LIB=$(pwd)/build/libsd.so \
  python3 tests/conformance/run_conformance.py
```

## Adding a function

1. Add a row to `coverage.toml` with the support level and case
   file path.
2. Drop the case file under `cases/`. Cases are tiny C programs
   that exit 0 on success and non-zero with a `errno` message on
   failure.
3. Update `docs/API.md` until the doc generator lands.

## What the suite does NOT cover

- Performance. The bench/microbench/ binaries handle that.
- Multi-host scenarios. Conformance is intra-host correctness only.
- LTP's POSIX-conformance subset that has no equivalent in
  practice (we cherry-pick what's relevant to networked apps).
