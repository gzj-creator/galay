add_executable(benchmark_c_rpc_envelope_codec b1_envelope_codec_smoke.c)
target_include_directories(benchmark_c_rpc_envelope_codec
    PRIVATE
        ${CMAKE_BINARY_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/c
)
target_link_libraries(benchmark_c_rpc_envelope_codec PRIVATE galay::c-rpc)
