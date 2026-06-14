if(TARGET galay::kernel)
    set(_galay_mcp_kernel_target galay::kernel)
elseif(TARGET galay-kernel)
    add_library(galay::kernel ALIAS galay-kernel)
    set(_galay_mcp_kernel_target galay::kernel)
elseif(TARGET kernel)
    add_library(galay::kernel ALIAS kernel)
    set(_galay_mcp_kernel_target galay::kernel)
elseif(TARGET galay-kernel::galay-kernel)
    set(_galay_mcp_kernel_target galay-kernel::galay-kernel)
else()
    message(FATAL_ERROR "mcp requires an in-tree kernel target")
endif()

if(TARGET galay::http)
    set(_galay_mcp_http_target galay::http)
elseif(TARGET http)
    add_library(galay::http ALIAS http)
    set(_galay_mcp_http_target galay::http)
elseif(TARGET galay-http)
    add_library(galay::http ALIAS galay-http)
    set(_galay_mcp_http_target galay::http)
elseif(TARGET galay-http::galay-http)
    set(_galay_mcp_http_target galay-http::galay-http)
else()
    message(FATAL_ERROR "mcp requires an in-tree http target")
endif()

galay_ensure_simdjson()

file(GLOB_RECURSE GALAY_MCP_SOURCES CONFIGURE_DEPENDS
    "common/*.cc"
    "client/*.cc"
    "server/*.cc"
)

add_library(mcp ${GALAY_MCP_SOURCES})
add_library(galay::mcp ALIAS mcp)

if(NOT TARGET galay-mcp)
    add_library(galay-mcp ALIAS mcp)
endif()
if(NOT TARGET galay-mcp::galay-mcp)
    add_library(galay-mcp::galay-mcp ALIAS mcp)
endif()

target_compile_features(mcp PUBLIC cxx_std_23)
target_include_directories(mcp
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(mcp
    PUBLIC
        ${_galay_mcp_kernel_target}
        ${_galay_mcp_http_target}
        simdjson::simdjson
)

set_target_properties(mcp PROPERTIES EXPORT_NAME mcp)
