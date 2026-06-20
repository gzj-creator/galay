add_executable(example_c_mcp_jsonrpc_builder e1_jsonrpc_builder.c)
target_include_directories(example_c_mcp_jsonrpc_builder
    PRIVATE
        ${CMAKE_BINARY_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/c
)
target_link_libraries(example_c_mcp_jsonrpc_builder PRIVATE galay::c-mcp)
