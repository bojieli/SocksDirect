#!/usr/bin/env bash
# repro_dry_run.sh — exercise every figure script in dry-run mode.
#
# Purpose: catch bit-rot in figure scripts even when the host doesn't
# have RDMA hardware. Each figure either runs end-to-end (Tier 1) or
# emits a DRYRUN sentinel CSV (Tier 2/3 figures requiring missing
# hardware/builds).
#
# CI runs this to make sure no figure script regresses to syntax-error
# territory unnoticed.

set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"

export REPRO_DRY_RUN=1
RESULTS_BASE="$(mktemp -d)"

trap 'rm -rf "$RESULTS_BASE"' EXIT

cd "$REPO"
python3 reproduce/repro check
echo
python3 reproduce/repro list
echo
python3 reproduce/repro --results-dir "$RESULTS_BASE" all

# Every figure should have produced *something* — either a real
# result.csv, a SKIPPED.txt, or a DRYRUN annotation.
fail=0
for fig in reproduce/figures/*/run.sh; do
    name=$(basename "$(dirname "$fig")")
    out="$RESULTS_BASE/$name"
    if [[ ! -d "$out" ]]; then
        echo "::error::no output dir for $name"
        fail=1
        continue
    fi
    if [[ ! -s "$out/result.csv" && ! -f "$out/SKIPPED.txt" ]]; then
        echo "::error::$name: no result.csv or SKIPPED.txt"
        fail=1
    fi
done

if [[ $fail -eq 0 ]]; then
    echo "all figures produced an artifact"
fi
exit $fail
