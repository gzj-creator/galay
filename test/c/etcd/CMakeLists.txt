add_executable(test_c_etcd_header_smoke t1_header_smoke.c)
target_include_directories(test_c_etcd_header_smoke
    PRIVATE
        ${CMAKE_BINARY_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/c
)
target_link_libraries(test_c_etcd_header_smoke PRIVATE galay::c-etcd)
add_test(NAME c.etcd.header_smoke COMMAND test_c_etcd_header_smoke)
set_tests_properties(c.etcd.header_smoke PROPERTIES LABELS "c;etcd;surface")

add_executable(test_c_etcd_sync_kv_surface t2_sync_kv_surface.c)
target_include_directories(test_c_etcd_sync_kv_surface
    PRIVATE
        ${CMAKE_BINARY_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/c
)
target_link_libraries(test_c_etcd_sync_kv_surface PRIVATE galay::c-etcd)
add_test(NAME c.etcd.sync_kv_surface COMMAND test_c_etcd_sync_kv_surface)
set_tests_properties(c.etcd.sync_kv_surface PROPERTIES LABELS "c;etcd;unit")
