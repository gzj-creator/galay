add_executable(example_c_redis_resp_command e1_resp_command.c)
target_include_directories(example_c_redis_resp_command
    PRIVATE
        ${CMAKE_BINARY_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/c
)
target_link_libraries(example_c_redis_resp_command PRIVATE galay::c-redis)
