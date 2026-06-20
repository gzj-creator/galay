add_library(galay-c-ssl ssl_context.cc)
add_library(galay::c-ssl ALIAS galay-c-ssl)

target_include_directories(galay-c-ssl
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(galay-c-ssl
    PUBLIC
        galay::c-common
        galay::ssl
)

set_target_properties(galay-c-ssl PROPERTIES EXPORT_NAME c-ssl)
