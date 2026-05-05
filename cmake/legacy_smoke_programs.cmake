# Legacy smoke programs preserved from the v0 prototype.
# These are NOT asserted tests; they're manually-driven sanity checks
# that the original authors used during development. They are gated
# behind -DSOCKSDIRECT_BUILD_LEGACY=ON because they hardcode behavior
# that the new tests/ subtree replaces with proper assertions.

set(_legacy_socket_smoke
    test/test_socket.c
    test/test_sock_server.cpp
    test/test_sock_client.c
    test/test_sock_pingpong_server.cpp
    test/test_sock_pingpong_client.cpp
    test/test_epoll_server.c
    test/test_epoll_client.c
    test/test_sock_server_2.cpp
    test/test_sock_client_2.c
    test/test_sock_server_3.cpp
    test/test_sock_client_3.c
    test/lat_sock_onetime_client.c
    test/lat_sock_onetime_server.cpp
    test/test_fork_client.c
    test/test_fork_server.c
    test/test_fork_client_2.c
    test/test_fork_server_2.c)

foreach(src ${_legacy_socket_smoke})
    get_filename_component(name ${src} NAME_WE)
    add_executable(legacy_${name} ${src})
    target_link_libraries(legacy_${name} sd)
endforeach()

add_executable(legacy_test_http_client test/test_http_client.c)
target_link_libraries(legacy_test_http_client Threads::Threads)

add_executable(legacy_test_memcached_client test/test_memcached_client.c common/helper.c)
target_link_libraries(legacy_test_memcached_client Threads::Threads)

add_executable(legacy_demo_cpu_util test/demo_cpu_util.c common/util.c)

add_executable(legacy_pot_web_service test/demo_web_service.c)
target_link_libraries(legacy_pot_web_service memcached Threads::Threads)
