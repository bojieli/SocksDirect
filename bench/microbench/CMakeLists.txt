# Standalone microbenchmarks — depend only on common/ headers and pthread.
# These are the inputs to the perf-regression gate in CI.

function(socksdirect_add_bench name)
    add_executable(${name} ${name}.cpp)
    target_include_directories(${name} PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(${name} PRIVATE Threads::Threads)
endfunction()

socksdirect_add_bench(bench_queue_v3)
socksdirect_add_bench(bench_fd_remap)
socksdirect_add_bench(bench_metrics)
socksdirect_add_bench(bench_config)
