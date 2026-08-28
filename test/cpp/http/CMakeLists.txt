file(GLOB GALAY_HTTP_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")

# These historical clients expect an independently managed HTTP server on
# 127.0.0.1:8080.  Keep their targets available for explicit fixture runs,
# but keep an unconfigured checkout's default CTest suite deterministic.
set(GALAY_HTTP_EXTERNAL_FIXTURE_TESTS
    t3_httpcl
    t4_awaitx
    t5_methods
    t10_http_client
    t11_timeout
)
# 是否启用需要本机 127.0.0.1:8080 外部服务的 HTTP 测试。
option(GALAY_HTTP_ENABLE_EXTERNAL_FIXTURE_TESTS
    "Enable HTTP tests requiring an external server on 127.0.0.1:8080"
    OFF
)

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
    # Most HTTP tests are self-contained.  The router/static-file tests create
    # relative files, so run them in the per-build test directory rather than
    # mutating the source checkout (and allow separate build trees to run in
    # parallel).
    set(test_working_directory "${PROJECT_SOURCE_DIR}")
    if(test_name STREQUAL "t8_mount" OR test_name STREQUAL "t9_static")
        set(test_working_directory "${CMAKE_CURRENT_BINARY_DIR}")
    endif()
    add_test(NAME "http.${scenario_name}"
        COMMAND ${CMAKE_COMMAND}
            -DTEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -DTEST_BINARY_NAME=${test_name}
            -DTEST_WORKING_DIRECTORY=${test_working_directory}
            -P ${PROJECT_SOURCE_DIR}/cmake/RunTestBinary.cmake
    )
    if(test_name IN_LIST GALAY_HTTP_EXTERNAL_FIXTURE_TESTS)
        set_tests_properties("http.${scenario_name}" PROPERTIES
            LABELS "http;integration;requires-http-fixture"
            RESOURCE_LOCK galay_loopback_port_8080
            SKIP_RETURN_CODE 125
            TIMEOUT 30
        )
        if(NOT GALAY_HTTP_ENABLE_EXTERNAL_FIXTURE_TESTS)
            set_tests_properties("http.${scenario_name}" PROPERTIES DISABLED TRUE)
        endif()
    else()
        set_tests_properties("http.${scenario_name}" PROPERTIES LABELS "http;unit")
    endif()
endforeach()
