add_executable(example_c_http2_frame_codec e1_frame_codec.c)
target_include_directories(example_c_http2_frame_codec
    PRIVATE
        ${CMAKE_BINARY_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/c
)
target_link_libraries(example_c_http2_frame_codec PRIVATE galay::c-http2)
