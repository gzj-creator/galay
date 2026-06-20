add_library(galay-c-tracing tracing.cc)
add_library(galay::c-tracing ALIAS galay-c-tracing)

target_compile_features(galay-c-tracing PUBLIC cxx_std_23)
target_include_directories(galay-c-tracing
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(galay-c-tracing
    PUBLIC
        galay::c-common
        galay::tracing
)

set_target_properties(galay-c-tracing PROPERTIES EXPORT_NAME c-tracing)
