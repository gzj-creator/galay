add_library(galay-c-mcp mcp.cc)
add_library(galay::c-mcp ALIAS galay-c-mcp)

target_compile_features(galay-c-mcp PUBLIC cxx_std_23)
target_include_directories(galay-c-mcp
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(galay-c-mcp
    PUBLIC
        galay::c-common
        galay::mcp
)

set_target_properties(galay-c-mcp PROPERTIES EXPORT_NAME c-mcp)
