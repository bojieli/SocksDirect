add_executable(rpclib_server server.c)
add_executable(rpclib_client client.c)
target_link_libraries(rpclib_server PRIVATE Threads::Threads)
target_link_libraries(rpclib_client PRIVATE Threads::Threads)
