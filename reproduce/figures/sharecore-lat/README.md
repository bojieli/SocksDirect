# sharecore-lat — share-core ping-pong latency

Paper figure "eval-context-switch" — N processes pinned to one core
exchange messages and we measure latency under contention. The paper's
claim depends on libsd's cooperative-multitasking design.

## Status

The original release does not contain the driver binary; this script
runs the queue-microbench latency mode pinned to core 0 as a proxy
that exercises the queue's path but not the cooperative-yield logic.
When the proper driver lands (see `docs/MISSING_FEATURES.md`), this
script switches to it.
