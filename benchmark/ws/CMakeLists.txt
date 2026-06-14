function(galay_add_ws_benchmark target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE galay::ws)
    target_include_directories(${target_name} PRIVATE
        ${PROJECT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/src
    )
    if(GALAY_HTTP_COROUTINE_WORKAROUND_ENABLED)
        target_compile_options(${target_name} PRIVATE ${GALAY_HTTP_COROUTINE_WORKAROUND_FLAGS})
    endif()
    set_target_properties(${target_name} PROPERTIES CXX_STANDARD 23 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
endfunction()

galay_add_ws_benchmark(benchmark_ws_ws_server_throughput b5_ws_server_throughput.cc)
galay_add_ws_benchmark(benchmark_ws_ws_client_throughput b6_ws_client_throughput.cc)
galay_add_ws_benchmark(benchmark_ws_wss_server_throughput b7_wss_server_throughput.cc)
galay_add_ws_benchmark(benchmark_ws_wss_client_throughput b8_wss_client_throughput.cc)
