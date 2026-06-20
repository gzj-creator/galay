add_library(galay-c-common common/galay_c_error.cc)
add_library(galay::c-common ALIAS galay-c-common)

target_include_directories(galay-c-common
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

set_target_properties(galay-c-common PROPERTIES EXPORT_NAME c-common)
