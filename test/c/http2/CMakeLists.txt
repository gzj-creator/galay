function(galay_add_c_http2_test target_name source_file)
    add_executable(${target_name} ${source_file})
    target_include_directories(${target_name}
        PRIVATE
            ${CMAKE_BINARY_DIR}/include
            ${CMAKE_SOURCE_DIR}/src/c
    )
    target_link_libraries(${target_name} PRIVATE galay::c-http2)
    add_test(NAME "c.http2.${target_name}" COMMAND ${target_name})
    set_tests_properties("c.http2.${target_name}" PROPERTIES LABELS "c;http2;unit")
endfunction()

galay_add_c_http2_test(header_smoke header_smoke.c)
galay_add_c_http2_test(frame_codec_test frame_codec_test.c)
galay_add_c_http2_test(hpack_test hpack_test.c)
galay_add_c_http2_test(settings_helpers_test settings_helpers_test.c)
