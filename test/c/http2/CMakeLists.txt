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

file(GLOB GALAY_C_HTTP2_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.c")
foreach(test_source IN LISTS GALAY_C_HTTP2_TEST_SOURCES)
    get_filename_component(test_name "${test_source}" NAME_WE)
    galay_add_c_http2_test(${test_name} ${test_source})
endforeach()
