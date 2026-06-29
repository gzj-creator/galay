file(GLOB galay_c_bridge_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/coro-c/*.cc"
)

add_library(galay-c-bridge ${galay_c_bridge_sources})
add_library(galay::c-bridge ALIAS galay-c-bridge)

target_compile_features(galay-c-bridge PUBLIC cxx_std_23)

target_include_directories(galay-c-bridge
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(galay-c-bridge
    PUBLIC
        galay::kernel
)

set_target_properties(galay-c-bridge PROPERTIES EXPORT_NAME c-bridge)
