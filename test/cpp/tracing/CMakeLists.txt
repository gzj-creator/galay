function(galay_add_tracing_test source_file)
    get_filename_component(test_name "${source_file}" NAME_WE)
    # t1_package_surface -> T1-package_surface (legacy target naming)
    string(REGEX REPLACE "^t([0-9]+)_(.+)$" "T\\1-\\2" target_name "${test_name}")
    string(REGEX REPLACE "^t[0-9]+_" "" scenario_name "${test_name}")

    add_executable(${target_name} ${source_file})
    target_include_directories(${target_name} PRIVATE ${CMAKE_BINARY_DIR}/include)

    if(test_name STREQUAL "t8_kernel_context")
        target_link_libraries(${target_name} PRIVATE galay-tracing::galay-tracing-kernel)
    elseif(test_name STREQUAL "t6_spdlog_sink")
        target_link_libraries(${target_name} PRIVATE galay-tracing::galay-tracing-spdlog)
    else()
        target_link_libraries(${target_name} PRIVATE galay-tracing::galay-tracing)
    endif()

    add_test(NAME tracing.${scenario_name} COMMAND ${target_name})
endfunction()

file(GLOB GALAY_TRACING_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")

foreach(test_source IN LISTS GALAY_TRACING_TEST_SOURCES)
    get_filename_component(test_name "${test_source}" NAME_WE)
    if(test_name STREQUAL "t6_spdlog_sink" AND NOT GALAY_TRACING_ENABLE_SPDLOG)
        continue()
    endif()
    galay_add_tracing_test(${test_source})
endforeach()
