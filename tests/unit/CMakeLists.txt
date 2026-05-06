# Unit tests — pure header-only modules from common/.
# These do not link libsd or libibverbs; they exercise data structures
# in isolation and run in milliseconds.

socksdirect_add_unit_test(test_darray            test_darray.cpp)
socksdirect_add_unit_test(test_adjlist           test_adjlist.cpp)
socksdirect_add_unit_test(test_locklessq_v3      test_locklessq_v3.cpp)
socksdirect_add_unit_test(test_locklessq_v2      test_locklessq_v2.cpp)
socksdirect_add_unit_test(test_fd_remap          test_fd_remap.cpp)
socksdirect_add_unit_test(test_helper_timing     test_helper_timing.cpp ${CMAKE_SOURCE_DIR}/common/helper.c)
target_link_libraries(test_helper_timing PRIVATE dl)
socksdirect_add_unit_test(test_config             test_config.cpp)
socksdirect_add_unit_test(test_log                test_log.cpp)
socksdirect_add_unit_test(test_metrics            test_metrics.cpp)
socksdirect_add_unit_test(test_monitor_ipc        test_monitor_ipc.cpp)
socksdirect_add_unit_test(test_zerocopy_client    test_zerocopy_client.cpp)
socksdirect_add_unit_test(test_zerocopy_mock      test_zerocopy_mock.cpp)
socksdirect_add_unit_test(test_shm_handshake      test_shm_handshake.cpp)
socksdirect_add_unit_test(test_shm_ring           test_shm_ring.cpp)
socksdirect_add_unit_test(test_shm_segment        test_shm_segment.cpp)
target_link_libraries(test_shm_segment PRIVATE rt)
socksdirect_add_unit_test(test_rdma_qp_info       test_rdma_qp_info.cpp)
