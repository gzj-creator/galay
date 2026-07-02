function(galay_add_c_common_test source_file)
    get_filename_component(scenario_name "${source_file}" NAME_WE)
    set(target_name "test_c_common_${scenario_name}")
    add_executable(${target_name} ${source_file})
    target_include_directories(${target_name}
        PRIVATE
            ${CMAKE_BINARY_DIR}/include
            ${CMAKE_SOURCE_DIR}/src/c
    )
    target_link_libraries(${target_name} PRIVATE galay::c-common)
    set_target_properties(${target_name} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
    if(scenario_name STREQUAL "no_exception_source_boundaries" OR
       scenario_name STREQUAL "c_abi_encapsulation_conventions")
        target_compile_definitions(${target_name} PRIVATE GALAY_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    endif()
    add_test(NAME "c.common.${scenario_name}" COMMAND ${target_name})
endfunction()

file(GLOB GALAY_C_COMMON_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.c")
foreach(test_source IN LISTS GALAY_C_COMMON_TEST_SOURCES)
    galay_add_c_common_test(${test_source})
endforeach()
