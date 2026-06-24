file(GLOB galay_c_kernel_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/core-c/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/async-c/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/concurrency-c/*.cc"
)

add_library(galay-c-kernel ${galay_c_kernel_sources})
add_library(galay::c-kernel ALIAS galay-c-kernel)

target_compile_features(galay-c-kernel PUBLIC cxx_std_23)

target_include_directories(galay-c-kernel
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(galay-c-kernel
    PUBLIC
        galay::kernel
)

set_target_properties(galay-c-kernel PROPERTIES EXPORT_NAME c-kernel)
