add_executable(example_c_rpc_envelope_codec e1_envelope_codec.c)
target_include_directories(example_c_rpc_envelope_codec
    PRIVATE
        ${CMAKE_BINARY_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/c
)
target_link_libraries(example_c_rpc_envelope_codec PRIVATE galay::c-rpc)
