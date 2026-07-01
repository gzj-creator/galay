file(GLOB_RECURSE GALAY_WS_SOURCES CONFIGURE_DEPENDS "*.cc")

add_library(galay-ws ${GALAY_WS_SOURCES})
add_library(galay::ws ALIAS galay-ws)

target_compile_features(galay-ws PUBLIC cxx_std_23)
target_include_directories(galay-ws
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(galay-ws
    PUBLIC
        galay::http
        galay::kernel
        galay::utils
)

if(GALAY_SSL_FEATURE_ENABLED)
    target_link_libraries(galay-ws PUBLIC galay::ssl)
    target_compile_definitions(galay-ws PUBLIC GALAY_SSL_FEATURE_ENABLED)
endif()

set_target_properties(galay-ws PROPERTIES EXPORT_NAME ws)
