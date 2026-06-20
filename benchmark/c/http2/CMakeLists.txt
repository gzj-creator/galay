add_executable(benchmark_c_http2_hpack_encode_smoke b1_hpack_encode_smoke.c)
target_include_directories(benchmark_c_http2_hpack_encode_smoke
    PRIVATE
        ${CMAKE_BINARY_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/c
)
target_link_libraries(benchmark_c_http2_hpack_encode_smoke PRIVATE galay::c-http2)
