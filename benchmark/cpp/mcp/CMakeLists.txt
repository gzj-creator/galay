function(galay_add_mcp_benchmark target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE galay::mcp)
    target_include_directories(${target_name} PRIVATE ${CMAKE_BINARY_DIR}/include)
    set_target_properties(${target_name} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
endfunction()

galay_add_mcp_benchmark(benchmark_mcp_stdio_request_throughput b1_stdio_request_throughput.cc)
galay_add_mcp_benchmark(benchmark_mcp_http_request_throughput b2_http_request_throughput.cc)
galay_add_mcp_benchmark(benchmark_mcp_http_concurrency_throughput b3_http_concurrency_throughput.cc)
galay_add_mcp_benchmark(benchmark_mcp_policy_defaults b4_policy_defaults.cc)
