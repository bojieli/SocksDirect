# nfv-pipeline — minimal NFV pipeline app

Five tiny Python programs piped together:

```
source.py | nf_firewall.py | nf_meter.py | nf_nat.py | sink.py
```

Each NF reads length-prefixed packets from stdin, updates a counter,
writes them to stdout. With `LD_PRELOAD=libsd.so` the inter-process
pipes ride on libsd's accelerated path.

## Run

```bash
./pipeline.sh --packets 100000
```

Output goes to stderr (per-NF counters); no stdout output.

## Why this exists

The paper's eval-tun-tput figure depends on an NFV pipeline app that
the original release never published (see `docs/MISSING_FEATURES.md`).
This skeleton is intentionally minimal — Python so it's readable, no
NF complexity beyond exercising the IPC path.

For real NFs (Click-style classifiers, BPF filters, etc.) replace
the .py programs with C versions — the pipeline driver doesn't care
what language the NFs are written in.
