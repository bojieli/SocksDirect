"""Integration tests for the reproduce/repro CLI.

The reproduction harness is the user-facing tool readers use to run the
paper figures. These tests exercise its surface — argument parsing,
inventory loading, tier detection, figure listing, run/report/clean —
without actually building libsd or running the real benchmarks.

We use a temp inventory + temp figures dir so each test is hermetic.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import textwrap
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
REPRO = REPO / "reproduce" / "repro"


def _run(cwd: Path, *args, env=None, timeout=20):
    base_env = os.environ.copy()
    if env:
        base_env.update(env)
    # Tests pass --figures-dir / --inventory / --results-dir explicitly so
    # the CLI never reads from the in-tree reproduce/.
    full = [
        "--figures-dir", str(cwd / "figures"),
        "--inventory",   str(cwd / "inventory.yml"),
        "--results-dir", str(cwd / "results"),
        *args,
    ]
    return subprocess.run(
        ["python3", str(REPRO), *full],
        cwd=str(cwd),
        env=base_env,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
    )


@pytest.fixture
def repro_workspace(tmp_path: Path):
    """Create an isolated reproduce/ tree pointed at tmp_path/results."""
    ws = tmp_path / "ws"
    ws.mkdir()

    figs = ws / "figures"
    figs.mkdir()

    # Two trivial figures so 'all' has something to iterate over.
    (figs / "alpha").mkdir()
    (figs / "alpha" / "run.sh").write_text(textwrap.dedent("""\
        #!/usr/bin/env bash
        set -euo pipefail
        echo "alpha-ran" > "$REPRO_RESULTS_DIR/result.csv"
    """))
    (figs / "alpha" / "run.sh").chmod(0o755)
    (figs / "alpha" / "README.md").write_text("# alpha figure\n\nA tiny figure.\n")

    (figs / "beta").mkdir()
    (figs / "beta" / "run.sh").write_text(textwrap.dedent("""\
        #!/usr/bin/env bash
        set -euo pipefail
        echo "beta-ran tier=$REPRO_TIER" > "$REPRO_RESULTS_DIR/result.csv"
        # Validate the harness passed sensible env.
        test -d "$REPRO_REPO"
        test -f "$REPRO_INVENTORY"
    """))
    (figs / "beta" / "run.sh").chmod(0o755)
    (figs / "beta" / "README.md").write_text("# beta figure\n")

    inv = ws / "inventory.yml"
    inv.write_text(textwrap.dedent("""\
        hosts:
          alpha:
            ssh: alpha.local
            cores: [0, 1]
            nic: mlx5_0
            rdma_ip: 10.0.0.1
        transport: rxe
        tier: 1
        results_dir: ./results
    """))

    # Symlink in the real repro CLI so it can find figures via
    # Path(__file__).resolve().parent.
    (ws / "repro").symlink_to(REPRO)
    return ws


def test_check_resolves_tier_and_lists_figures(repro_workspace):
    r = _run(repro_workspace, "check")
    assert r.returncode == 0, r.stderr
    out = r.stdout.decode()
    assert "Resolved tier: 1" in out
    assert "alpha" in out
    assert "beta" in out


def test_list_prints_figure_titles(repro_workspace):
    r = _run(repro_workspace, "list")
    assert r.returncode == 0, r.stderr
    out = r.stdout.decode()
    # Title is the first line of README minus the leading hash.
    assert "alpha figure" in out
    assert "beta figure" in out


def test_run_single_figure(repro_workspace):
    r = _run(repro_workspace, "alpha")
    assert r.returncode == 0, r.stderr
    csv = repro_workspace / "results" / "alpha" / "result.csv"
    assert csv.exists()
    assert "alpha-ran" in csv.read_text()


def test_run_all_runs_every_figure(repro_workspace):
    r = _run(repro_workspace, "all")
    assert r.returncode == 0, r.stderr
    assert (repro_workspace / "results" / "alpha" / "result.csv").exists()
    assert (repro_workspace / "results" / "beta"  / "result.csv").exists()
    out = r.stdout.decode()
    assert "2/2 figures succeeded" in out


def test_unknown_figure_is_rejected(repro_workspace):
    r = _run(repro_workspace, "nope")
    # Argparse prints to stderr and exits 2 on unknown subcommand.
    assert r.returncode == 2
    assert b"invalid choice" in r.stderr or b"argument" in r.stderr


def test_failing_figure_makes_all_exit_nonzero(repro_workspace):
    # Replace alpha's run.sh with one that fails.
    (repro_workspace / "figures" / "alpha" / "run.sh").write_text(
        "#!/usr/bin/env bash\nexit 7\n")
    r = _run(repro_workspace, "all")
    assert r.returncode != 0
    out = r.stdout.decode()
    assert "1/2 figures succeeded" in out


def test_report_writes_summary_with_tier_banner(repro_workspace):
    _run(repro_workspace, "alpha")
    r = _run(repro_workspace, "report")
    assert r.returncode == 0, r.stderr
    summary = (repro_workspace / "results" / "summary.md").read_text()
    assert "Resolved tier" in summary
    assert "Tier 1" in summary  # the banner
    assert "alpha" in summary
    # alpha was run, beta wasn't.
    assert "_not run_" in summary


def test_clean_removes_results(repro_workspace):
    _run(repro_workspace, "alpha")
    assert (repro_workspace / "results").exists()
    r = _run(repro_workspace, "clean")
    assert r.returncode == 0
    assert not (repro_workspace / "results").exists()


def test_missing_inventory_dies_with_clear_message(repro_workspace):
    (repro_workspace / "inventory.yml").unlink()
    r = _run(repro_workspace, "check")
    assert r.returncode != 0
    assert b"inventory" in r.stderr.lower()


def test_inventory_environment_passed_to_figure(repro_workspace):
    # beta's run.sh asserts REPRO_REPO/REPRO_INVENTORY/REPRO_TIER are set.
    r = _run(repro_workspace, "beta")
    assert r.returncode == 0, r.stderr
    text = (repro_workspace / "results" / "beta" / "result.csv").read_text()
    assert "tier=1" in text


def test_tier_forced_via_inventory_overrides_detection(repro_workspace):
    # Force tier 3 in inventory; check should report it.
    inv = (repro_workspace / "inventory.yml").read_text().replace("tier: 1", "tier: 3")
    (repro_workspace / "inventory.yml").write_text(inv)
    r = _run(repro_workspace, "check")
    assert r.returncode == 0, r.stderr
    assert "Resolved tier: 3" in r.stdout.decode()
