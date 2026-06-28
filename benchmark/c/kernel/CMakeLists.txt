find_package(Threads REQUIRED)

function(galay_add_c_kernel_benchmark target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE galay::c-kernel Threads::Threads)
    target_include_directories(${target_name}
        PRIVATE
            ${CMAKE_BINARY_DIR}/include
            ${CMAKE_SOURCE_DIR}/src/c
    )
    set_target_properties(${target_name} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
endfunction()

file(GLOB GALAY_C_KERNEL_BENCHMARK_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.c")
foreach(benchmark_source IN LISTS GALAY_C_KERNEL_BENCHMARK_SOURCES)
    get_filename_component(source_name "${benchmark_source}" NAME_WE)
    if((source_name STREQUAL "b19_coro_yield_requeue_latency" OR
        source_name STREQUAL "b20_coro_wait_result_latency" OR
        source_name STREQUAL "b21_coro_tcp_echo_throughput" OR
        source_name STREQUAL "b22_coro_tcp_vs_callback") AND NOT GALAY_C_CORO_CONTEXT_SUPPORTED)
        message(STATUS "Skipping ${source_name}: ${GALAY_C_CORO_CONTEXT_UNSUPPORTED_REASON}")
        continue()
    endif()
    string(REGEX REPLACE "^b[0-9]+_" "" scenario_name "${source_name}")
    galay_add_c_kernel_benchmark("benchmark_c_kernel_${scenario_name}" "${benchmark_source}")
endforeach()
