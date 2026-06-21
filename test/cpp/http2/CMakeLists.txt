file(GLOB GALAY_HTTP2_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")

foreach(test_source ${GALAY_HTTP2_TEST_SOURCES})
    get_filename_component(test_name "${test_source}" NAME_WE)
    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} PRIVATE galay::http2)
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
    add_test(NAME "http2.${scenario_name}"
        COMMAND ${CMAKE_COMMAND}
            -DTEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -DTEST_BINARY_NAME=${test_name}
            -DTEST_WORKING_DIRECTORY=${PROJECT_SOURCE_DIR}
            -P ${PROJECT_SOURCE_DIR}/cmake/RunTestBinary.cmake
    )
    set_tests_properties("http2.${scenario_name}" PROPERTIES LABELS "http2;unit")
endforeach()
