add_executable(test_c_ws_frame_codec t_ws_frame_codec.c)
target_include_directories(test_c_ws_frame_codec
    PRIVATE
        ${CMAKE_BINARY_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/c
)
target_link_libraries(test_c_ws_frame_codec PRIVATE galay::c-ws)
add_test(NAME c.ws.frame_codec COMMAND test_c_ws_frame_codec)
set_tests_properties(c.ws.frame_codec PROPERTIES LABELS "c;ws;unit")

