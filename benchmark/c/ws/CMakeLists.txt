add_executable(benchmark_c_ws_frame_codec frame_codec_throughput.c)
target_include_directories(benchmark_c_ws_frame_codec
    PRIVATE
        ${CMAKE_BINARY_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/c
)
target_link_libraries(benchmark_c_ws_frame_codec PRIVATE galay::c-ws)

