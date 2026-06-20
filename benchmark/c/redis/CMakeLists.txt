add_executable(benchmark_c_redis_resp_codec_smoke resp_codec_smoke.c)
target_include_directories(benchmark_c_redis_resp_codec_smoke
    PRIVATE
        ${CMAKE_BINARY_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/c
)
target_link_libraries(benchmark_c_redis_resp_codec_smoke PRIVATE galay::c-redis)
