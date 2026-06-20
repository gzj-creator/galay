add_library(galay-c-http2 http2.cc)
add_library(galay::c-http2 ALIAS galay-c-http2)

target_compile_features(galay-c-http2 PRIVATE cxx_std_23)
target_include_directories(galay-c-http2
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/src/c>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(galay-c-http2
    PUBLIC
        galay::c-common
    PRIVATE
        galay::http2
)

set_target_properties(galay-c-http2 PROPERTIES EXPORT_NAME c-http2)
