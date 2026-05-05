# Baseline benchmarks — Linux primitives, no libsd, no RDMA.
# Used by the reproduction harness as the reference numbers
# intra-host figures compare against.

add_executable(linux_tcp_pingpong linux_tcp_pingpong.c)
target_include_directories(linux_tcp_pingpong PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(linux_tcp_pingpong PRIVATE Threads::Threads)
