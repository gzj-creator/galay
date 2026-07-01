file(GLOB_RECURSE GALAY_HTTP2_SOURCES CONFIGURE_DEPENDS "*.cc")

add_library(galay-http2 ${GALAY_HTTP2_SOURCES})
add_library(galay::http2 ALIAS galay-http2)

target_compile_features(galay-http2 PUBLIC cxx_std_23)
target_include_directories(galay-http2
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(galay-http2
    PUBLIC
        galay::http
        galay::kernel
        galay::utils
)

if(GALAY_SSL_FEATURE_ENABLED)
    target_link_libraries(galay-http2 PUBLIC galay::ssl)
    target_compile_definitions(galay-http2 PUBLIC GALAY_SSL_FEATURE_ENABLED)
endif()

set_target_properties(galay-http2 PROPERTIES EXPORT_NAME http2)
