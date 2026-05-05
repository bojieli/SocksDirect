# Benchmark binaries — both microbenchmarks (queue / SHM only) and
# end-to-end ones that link libsd.

add_subdirectory(microbench)

# Per-figure benchmarks live under intra/ and inter/ but they require
# libsd + RDMA which is gated higher up.
