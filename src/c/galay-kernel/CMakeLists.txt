add_library(galay-c-kernel galay_kernel.cc)
add_library(galay::c-kernel ALIAS galay-c-kernel)

target_link_libraries(galay-c-kernel
    PUBLIC
        galay::c-common
        galay::kernel
)

target_include_directories(galay-c-kernel
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

set_target_properties(galay-c-kernel PROPERTIES
    EXPORT_NAME c-kernel
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)
