add_executable(test_c_redis_header_smoke t1_header_smoke.c)
target_include_directories(test_c_redis_header_smoke
    PRIVATE
        ${CMAKE_BINARY_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/c
)
target_link_libraries(test_c_redis_header_smoke PRIVATE galay::c-redis)
add_test(NAME c.redis.header_smoke COMMAND test_c_redis_header_smoke)
set_tests_properties(c.redis.header_smoke PROPERTIES LABELS "c;redis;surface")

add_executable(test_c_redis_resp_surface t2_resp_surface.c)
target_include_directories(test_c_redis_resp_surface
    PRIVATE
        ${CMAKE_BINARY_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/c
)
target_link_libraries(test_c_redis_resp_surface PRIVATE galay::c-redis)
add_test(NAME c.redis.resp_surface COMMAND test_c_redis_resp_surface)
set_tests_properties(c.redis.resp_surface PROPERTIES LABELS "c;redis;unit")

add_executable(test_c_redis_client_boundary t3_client_boundary.c)
target_include_directories(test_c_redis_client_boundary
    PRIVATE
        ${CMAKE_BINARY_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/c
)
target_link_libraries(test_c_redis_client_boundary PRIVATE galay::c-redis)
add_test(NAME c.redis.client_boundary COMMAND test_c_redis_client_boundary)
set_tests_properties(c.redis.client_boundary PROPERTIES LABELS "c;redis;unit")
