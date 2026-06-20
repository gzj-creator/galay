add_executable(test_c_mcp_header_smoke t1_header_smoke.c)
target_include_directories(test_c_mcp_header_smoke
    PRIVATE
        ${CMAKE_BINARY_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/c
)
target_link_libraries(test_c_mcp_header_smoke PRIVATE galay::c-mcp)
add_test(NAME c.mcp.header_smoke COMMAND test_c_mcp_header_smoke)
set_tests_properties(c.mcp.header_smoke PROPERTIES LABELS "c;mcp;surface")

add_executable(test_c_mcp_jsonrpc_surface t2_jsonrpc_surface.c)
target_include_directories(test_c_mcp_jsonrpc_surface
    PRIVATE
        ${CMAKE_BINARY_DIR}/include
        ${CMAKE_SOURCE_DIR}/src/c
)
target_link_libraries(test_c_mcp_jsonrpc_surface PRIVATE galay::c-mcp)
add_test(NAME c.mcp.jsonrpc_surface COMMAND test_c_mcp_jsonrpc_surface)
set_tests_properties(c.mcp.jsonrpc_surface PROPERTIES LABELS "c;mcp;unit")
