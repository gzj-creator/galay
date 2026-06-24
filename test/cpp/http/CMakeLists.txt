file(GLOB GALAY_HTTP_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")

foreach(test_source ${GALAY_HTTP_TEST_SOURCES})
    get_filename_component(test_name "${test_source}" NAME_WE)
    file(READ "${test_source}" test_source_content)
    set(test_requires_http2 FALSE)
    set(test_requires_ws FALSE)
    if(test_source_content MATCHES "#include[^\n]+[<\"]galay/cpp/galay-http2/")
        set(test_requires_http2 TRUE)
    endif()
    if(test_source_content MATCHES "#include[^\n]+[<\"]galay/cpp/galay-ws/")
        set(test_requires_ws TRUE)
    endif()
    if(test_requires_http2 AND NOT GALAY_BUILD_HTTP2)
        continue()
    endif()
    if(test_requires_ws AND NOT GALAY_BUILD_WS)
        continue()
    endif()
    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} PRIVATE galay::http)
    if(test_requires_http2)
        target_link_libraries(${test_name} PRIVATE galay::http2)
    endif()
    if(test_requires_ws)
        target_link_libraries(${test_name} PRIVATE galay::ws)
    endif()
    target_include_directories(${test_name} PRIVATE ${CMAKE_BINARY_DIR}/include)
    if(GALAY_HTTP_COROUTINE_WORKAROUND_ENABLED)
        target_compile_options(${test_name} PRIVATE ${GALAY_HTTP_COROUTINE_WORKAROUND_FLAGS})
    endif()
    target_compile_definitions(${test_name} PRIVATE USE_LOG)
    set_target_properties(${test_name} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        INSTALL_RPATH $ORIGIN/../lib
    )
    string(REGEX REPLACE "^t[0-9]+_" "" scenario_name "${test_name}")
    add_test(NAME "http.${scenario_name}"
        COMMAND ${CMAKE_COMMAND}
            -DTEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -DTEST_BINARY_NAME=${test_name}
            -DTEST_WORKING_DIRECTORY=${PROJECT_SOURCE_DIR}
            -P ${PROJECT_SOURCE_DIR}/cmake/RunTestBinary.cmake
    )
    set_tests_properties("http.${scenario_name}" PROPERTIES LABELS "http;unit")
endforeach()
