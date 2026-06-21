file(GLOB GALAY_MCP_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")

foreach(test_source IN LISTS GALAY_MCP_TEST_SOURCES)
    get_filename_component(test_name "${test_source}" NAME_WE)
    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} PRIVATE galay::mcp)
    target_include_directories(${test_name} PRIVATE ${CMAKE_BINARY_DIR}/include)
    set_target_properties(${test_name} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
    string(REGEX REPLACE "^t[0-9]+_" "" scenario_name "${test_name}")
    set(ctest_name "mcp.${scenario_name}")
    add_test(NAME ${ctest_name} COMMAND $<TARGET_FILE:${test_name}>)
    if(test_name STREQUAL "t1_await")
        set_tests_properties(${ctest_name} PROPERTIES LABELS "mcp;surface")
    else()
        set_tests_properties(${ctest_name} PROPERTIES LABELS "mcp;unit")
    endif()
endforeach()
