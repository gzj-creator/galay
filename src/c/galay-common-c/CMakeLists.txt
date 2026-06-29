file(GLOB galay_c_common_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cc"
)

add_library(galay-c-common ${galay_c_common_sources})
add_library(galay::c-common ALIAS galay-c-common)

target_compile_features(galay-c-common PUBLIC cxx_std_23)
target_include_directories(galay-c-common
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

set_target_properties(galay-c-common PROPERTIES EXPORT_NAME c-common)
