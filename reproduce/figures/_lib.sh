# Shared helpers for reproduce/figures/<name>/run.sh scripts.
#
# Source this from each figure's run.sh:
#
#   . "$(dirname "$0")/../_lib.sh"
#   require_repro_env
#
# It validates the harness env (REPRO_REPO, REPRO_RESULTS_DIR, REPRO_TIER)
# and exposes helpers for skipping cleanly when a prerequisite is missing.

set -euo pipefail

require_repro_env() {
    : "${REPRO_REPO:?must be set by ./repro}"
    : "${REPRO_RESULTS_DIR:?must be set by ./repro}"
    : "${REPRO_TIER:?must be set by ./repro}"
    mkdir -p "$REPRO_RESULTS_DIR"
}

# Skip with exit code 0 + a sentinel file the report can read.
skip_figure() {
    local reason="$1"
    : "${REPRO_RESULTS_DIR:?}"
    mkdir -p "$REPRO_RESULTS_DIR"
    echo "$reason" > "$REPRO_RESULTS_DIR/SKIPPED.txt"
    echo "skipped: $reason"
    exit 0
}

# Bail if libsd hasn't been built. Pass the figure's required tier to
# distinguish between "we're at Tier 1 deliberately" vs "you forgot to
# build libsd".
require_libsd() {
    local needed_tier="${1:-2}"
    if [[ "${REPRO_TIER}" -lt "${needed_tier}" ]]; then
        skip_figure "tier ${REPRO_TIER} cannot exercise this figure (needs tier ${needed_tier}+)"
    fi
    if [[ ! -x "${REPRO_REPO}/build/libsd.so" ]] && [[ ! -f "${REPRO_REPO}/build/libsd.so" ]]; then
        skip_figure "libsd.so not built; configure with -DSOCKSDIRECT_WITH_RDMA=ON -DSOCKSDIRECT_WITH_HERD=ON"
    fi
}

require_two_hosts() {
    if [[ "${REPRO_TIER}" -lt 3 ]]; then
        if [[ "${REPRO_DRY_RUN:-0}" == "1" ]]; then
            # Dry-run mode: emit a sentinel CSV that validates the
            # figure script is wired correctly, without trying to
            # actually run anything. Used by CI to catch script
            # bit-rot when no hardware is available.
            : "${REPRO_RESULTS_DIR:?}"
            mkdir -p "$REPRO_RESULTS_DIR"
            cat > "$REPRO_RESULTS_DIR/result.csv" <<EOF_CSV
iter,bench,submode,mode,msg_size,throughput_mps,p50_ns
DRYRUN,$(basename "$(dirname "${BASH_SOURCE[1]}")"),dry-run,annotation,,,
EOF_CSV
            echo "DRYRUN: $(basename "$(dirname "${BASH_SOURCE[1]}")") would run on Tier 3 hardware"
            exit 0
        fi
        skip_figure "inter-host figure requires Tier 3 (real RDMA on two hosts)"
    fi
}

require_command() {
    local cmd="$1" pkg="${2:-}"
    if ! command -v "$cmd" >/dev/null 2>&1; then
        local hint=""
        [[ -n "$pkg" ]] && hint=" (apt install $pkg)"
        skip_figure "missing dependency: $cmd$hint"
    fi
}

require_bench_binary() {
    local name="$1"
    local p="${REPRO_REPO}/build/bench/${name}"
    if [[ ! -x "$p" ]]; then
        # Try the microbench path too.
        local p2="${REPRO_REPO}/build/bench/microbench/${name}"
        if [[ ! -x "$p2" ]]; then
            skip_figure "missing bench binary: $name (run cmake --build build -j)"
        fi
    fi
}

# Convert raw.jsonl in REPRO_RESULTS_DIR to a flat result.csv with the
# columns: iter, bench, submode, mode, msg_size, throughput_mps, p50_ns.
# Most figures share this format; the plot templates assume it.
emit_csv() {
    local raw="${REPRO_RESULTS_DIR}/raw.jsonl"
    local csv="${REPRO_RESULTS_DIR}/result.csv"
    [[ -s "$raw" ]] || { echo "iter,bench,submode,mode,msg_size,throughput_mps,p50_ns" > "$csv"; return; }
    python3 - "$raw" "$csv" <<'PY'
import json, csv, sys
src, dst = sys.argv[1], sys.argv[2]
rows = [("iter","bench","submode","mode","msg_size","throughput_mps","p50_ns")]
counts = {}
with open(src) as fh:
    for line in fh:
        line = line.strip()
        if not line: continue
        o = json.loads(line)
        sub = o.get("submode","")
        msgsize = sub.split("=",1)[1] if "=" in sub else ""
        key = (o["bench"], sub, o["mode"])
        counts[key] = counts.get(key, 0) + 1
        rows.append((
            str(counts[key]), o["bench"], sub, o["mode"],
            msgsize,
            "" if o.get("throughput_mps") is None else f"{o['throughput_mps']:.2f}",
            "" if o.get("p50_ns") is None else str(o["p50_ns"]),
        ))
with open(dst, "w", newline="") as fh:
    csv.writer(fh).writerows(rows)
print(f"wrote {dst}")
PY
}

# If gnuplot is on PATH, render a simple PDF using a generic template.
maybe_plot() {
    local kind="${1:-line}"   # line | bar
    local title="${2:-figure}"
    if ! command -v gnuplot >/dev/null 2>&1; then
        echo "(gnuplot not installed; skipping PDF render)"
        return
    fi
    local csv="${REPRO_RESULTS_DIR}/result.csv"
    local pdf="${REPRO_RESULTS_DIR}/result.pdf"
    [[ -s "$csv" ]] || return
    local tmpl="${REPRO_REPO}/reproduce/plot-templates/${kind}.plt"
    [[ -f "$tmpl" ]] || return
    gnuplot -e "csv='$csv'; out='$pdf'; title='$title'" "$tmpl" >/dev/null 2>&1 \
        && echo "rendered $pdf" \
        || echo "(gnuplot render failed; CSV is the source of truth)"
}
