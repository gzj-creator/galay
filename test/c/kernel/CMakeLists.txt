function(galay_add_c_kernel_test target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE galay::c-kernel)
    target_include_directories(${target_name}
        PRIVATE
            ${CMAKE_BINARY_DIR}/include
            ${CMAKE_SOURCE_DIR}/src/c
    )
    set_target_properties(${target_name} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
endfunction()

file(GLOB GALAY_C_KERNEL_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.c")
foreach(test_source IN LISTS GALAY_C_KERNEL_TEST_SOURCES)
    get_filename_component(source_name "${test_source}" NAME_WE)
    if((source_name STREQUAL "t23_coro_task" OR
        source_name STREQUAL "t24_coro_result_wait" OR
        source_name STREQUAL "t25_coro_tcp") AND NOT GALAY_C_CORO_CONTEXT_SUPPORTED)
        message(STATUS "Skipping ${source_name}: ${GALAY_C_CORO_CONTEXT_UNSUPPORTED_REASON}")
        continue()
    endif()
    string(REGEX REPLACE "^t[0-9]+_" "" scenario_name "${source_name}")
    if(source_name STREQUAL "t22_coro_source_boundaries")
        set(scenario_name "${source_name}")
    endif()
    set(target_name "test_c_kernel_${scenario_name}")
    galay_add_c_kernel_test(${target_name} ${test_source})
    if(source_name STREQUAL "t22_coro_source_boundaries")
        target_compile_definitions(${target_name} PRIVATE GALAY_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    endif()
    add_test(NAME "c.kernel.${scenario_name}" COMMAND ${target_name})
    set_tests_properties("c.kernel.${scenario_name}" PROPERTIES LABELS "c;kernel;unit")
endforeach()
