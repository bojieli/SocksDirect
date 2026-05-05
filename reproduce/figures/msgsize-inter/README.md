# msgsize-inter — inter-host throughput vs message size

Reproduces the inter-host curve in paper Figure 3-right.

## Tier

**Tier 3 only.** Refuses to run on SoftRoCE (the harness skips and
the report banner explains why). See `docs/REPRODUCIBILITY.md`.

## Requirements

- Two hosts in `inventory.yml`, each with a Mellanox NIC and a
  configured RoCEv2 GID.
- SSH access from the driver host to the peer (no password prompts).
- libsd built on both hosts at the same path (`remote_repo_path`).

## Output

Same schema as `msgsize-intra`. Inter-host CSVs are tagged
`transport=rdma` (or `transport=rxe` if SoftRoCE was used — these
are excluded from the comparison report).
