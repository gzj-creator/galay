file(GLOB galay_c_utils_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cc"
)

add_library(galay-c-utils ${galay_c_utils_sources})
add_library(galay::c-utils ALIAS galay-c-utils)

target_compile_features(galay-c-utils PUBLIC cxx_std_23)
target_include_directories(galay-c-utils
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(galay-c-utils PUBLIC galay::c-common galay::utils)

set_target_properties(galay-c-utils PROPERTIES EXPORT_NAME c-utils)
