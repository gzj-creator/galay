function(galay_add_c_ws_benchmark target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE galay::c-ws)
    target_include_directories(${target_name}
        PRIVATE
            ${CMAKE_BINARY_DIR}/include
            ${CMAKE_SOURCE_DIR}/src/c
    )
    set_target_properties(${target_name} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
endfunction()

file(GLOB GALAY_C_WS_BENCHMARK_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.c")
foreach(benchmark_source IN LISTS GALAY_C_WS_BENCHMARK_SOURCES)
    get_filename_component(source_name "${benchmark_source}" NAME_WE)
    string(REGEX REPLACE "^b[0-9]+_" "" scenario_name "${source_name}")
    galay_add_c_ws_benchmark("benchmark_c_ws_${scenario_name}" "${benchmark_source}")
endforeach()
