add_executable(test_c_rpc_envelope_codec t1_envelope_codec.c)
target_include_directories(test_c_rpc_envelope_codec
    PRIVATE
        ${CMAKE_BINARY_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/c
)
target_link_libraries(test_c_rpc_envelope_codec PRIVATE galay::c-rpc)
add_test(NAME c.rpc.envelope_codec COMMAND test_c_rpc_envelope_codec)
set_tests_properties(c.rpc.envelope_codec PROPERTIES LABELS "c;rpc;unit")
