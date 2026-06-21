function(galay_add_utils_test target_name source_file)
    add_executable("${target_name}" "${source_file}")
    target_link_libraries("${target_name}" PRIVATE galay::utils)
    target_include_directories("${target_name}" PRIVATE ${CMAKE_BINARY_DIR}/include)
    if(target_name STREQUAL "t6_buffer_queue_ring" OR
       target_name STREQUAL "t7_cache_lru" OR
       target_name STREQUAL "t8_concurrency_pool_waiter" OR
       target_name STREQUAL "t12_resilience_rate_circuit")
        target_compile_definitions("${target_name}" PRIVATE
            GALAY_UTILS_SOURCE_DIR="${PROJECT_SOURCE_DIR}/src/cpp"
        )
    endif()
    string(REGEX REPLACE "^t[0-9]+_" "" scenario_name "${target_name}")
    string(REGEX REPLACE "^t_" "" scenario_name "${scenario_name}")
    string(REGEX REPLACE "_test$" "" scenario_name "${scenario_name}")
    add_test(NAME "utils.${scenario_name}" COMMAND "${target_name}")
    set_tests_properties("utils.${scenario_name}" PROPERTIES LABELS "utils;unit")
endfunction()

# Auto-discover numbered tests t{n}_*.cc; t_import_smoke is handled separately below.
file(GLOB GALAY_UTILS_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/t[0-9]*.cc")

foreach(test_source IN LISTS GALAY_UTILS_TEST_SOURCES)
    get_filename_component(test_name "${test_source}" NAME_WE)
    galay_add_utils_test("${test_name}" "${test_source}")
endforeach()

if(GALAY_ENABLE_CPP23_MODULES AND TARGET galay-utils-modules)
    galay_add_utils_test(t_import_smoke t_import_smoke.cc)
    target_link_libraries(t_import_smoke PRIVATE galay-utils-modules)
    set_target_properties(t_import_smoke PROPERTIES CXX_SCAN_FOR_MODULES ON)
    set_tests_properties(utils.import_smoke PROPERTIES LABELS "utils;surface")
endif()
