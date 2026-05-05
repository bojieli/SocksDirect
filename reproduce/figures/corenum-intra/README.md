# corenum-intra — intra-host scalability

How throughput scales as concurrent connections move to more cores.
Reproduces the IPC curve in paper Fig 4-left.

**Tier 2.** Skips otherwise.

Override the size / max cores via `SIZE=64 MAX_CORES=12 ./reproduce/repro corenum-intra`.
