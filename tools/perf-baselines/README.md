# perf-baselines/

Baseline JSONL files consumed by `tools/perf_regression.py` and the
`.github/workflows/perf-regression.yml` self-hosted gate.

Refresh manually:

```bash
gh workflow run perf-regression.yml -f refresh_baseline=true
```

This re-runs the suite and commits the new baseline. Don't refresh
on every change — the floor should move only when we *intend* to
move it.
