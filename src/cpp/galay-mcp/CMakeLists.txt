galay_ensure_simdjson()

file(GLOB_RECURSE GALAY_MCP_SOURCES CONFIGURE_DEPENDS
    "common/*.cc"
    "v1/client/*.cc"
    "v1/server/*.cc"
    "v2/common/*.cc"
    "v2/client/*.cc"
    "v2/server/*.cc"
)

add_library(galay-mcp ${GALAY_MCP_SOURCES})
add_library(galay::mcp ALIAS galay-mcp)

target_compile_features(galay-mcp PUBLIC cxx_std_23)
target_include_directories(galay-mcp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(galay-mcp
    PUBLIC
        galay::kernel
        galay::http
        simdjson::simdjson
)

set_target_properties(galay-mcp PROPERTIES EXPORT_NAME mcp)
