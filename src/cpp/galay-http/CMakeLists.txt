file(GLOB_RECURSE GALAY_HTTP_SOURCES CONFIGURE_DEPENDS "*.cc")

add_library(galay-http ${GALAY_HTTP_SOURCES})
add_library(galay::http ALIAS galay-http)

target_compile_features(galay-http PUBLIC cxx_std_23)
target_include_directories(galay-http
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(galay-http
    PUBLIC
        galay::kernel
        galay::utils
)

if(GALAY_SSL_FEATURE_ENABLED)
    target_link_libraries(galay-http PUBLIC galay::ssl)
    target_compile_definitions(galay-http PUBLIC GALAY_SSL_FEATURE_ENABLED)
endif()

set_target_properties(galay-http PROPERTIES EXPORT_NAME http)
