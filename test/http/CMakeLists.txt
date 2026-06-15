file(GLOB GALAY_HTTP_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")

set(GALAY_HTTP_MANUAL_TEST_TARGETS
    t3_rw
    t4_rw
    t5_http
    t9_chunk
    t10_chunk
    t15_range_etag
    t21_https
    t22_https
    t23_https
    t24_https
)

foreach(test_source ${GALAY_HTTP_TEST_SOURCES})
    get_filename_component(test_name "${test_source}" NAME_WE)
    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} PRIVATE galay::http)
    file(READ "${test_source}" test_source_content)
    if(test_source_content MATCHES "#include[^\n]+\"galay-http2/")
        target_link_libraries(${test_name} PRIVATE galay::http2)
    endif()
    if(test_source_content MATCHES "#include[^\n]+\"galay-ws/")
        target_link_libraries(${test_name} PRIVATE galay::ws)
    endif()
    target_include_directories(${test_name} PRIVATE ${PROJECT_SOURCE_DIR}/src)
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
    list(FIND GALAY_HTTP_MANUAL_TEST_TARGETS ${test_name} manual_index)
    if(manual_index EQUAL -1)
        add_test(NAME "http.${scenario_name}"
            COMMAND ${CMAKE_COMMAND}
                -DTEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
                -DTEST_BINARY_NAME=${test_name}
                -DTEST_WORKING_DIRECTORY=${PROJECT_SOURCE_DIR}
                -P ${PROJECT_SOURCE_DIR}/cmake/RunTestBinary.cmake
        )
        set_tests_properties("http.${scenario_name}" PROPERTIES LABELS "http;unit")
    endif()
endforeach()

if(GALAY_ENABLE_CPP23_MODULES AND TARGET galay-http-modules)
    add_executable(t59_module_smoke t59_module.cc)
    target_link_libraries(t59_module_smoke PRIVATE galay-http-modules)
    target_include_directories(t59_module_smoke PRIVATE ${PROJECT_SOURCE_DIR}/src)
    set_target_properties(t59_module_smoke PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        CXX_SCAN_FOR_MODULES ON
    )
    add_test(NAME http.module_smoke
        COMMAND ${CMAKE_COMMAND}
            -DTEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -DTEST_BINARY_NAME=t59_module_smoke
            -DTEST_WORKING_DIRECTORY=${PROJECT_SOURCE_DIR}
            -P ${PROJECT_SOURCE_DIR}/cmake/RunTestBinary.cmake
    )
    set_tests_properties(http.module_smoke PROPERTIES LABELS "http;surface")
endif()
