add_executable(benchmark_c_mcp_jsonrpc_builder_smoke b1_jsonrpc_builder_smoke.c)
target_include_directories(benchmark_c_mcp_jsonrpc_builder_smoke
    PRIVATE
        ${CMAKE_BINARY_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/c
)
target_link_libraries(benchmark_c_mcp_jsonrpc_builder_smoke PRIVATE galay::c-mcp)
