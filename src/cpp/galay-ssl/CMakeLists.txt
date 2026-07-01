file(GLOB_RECURSE SSL_SOURCES CONFIGURE_DEPENDS
    "*.cc"
    "*.h"
    "*.hpp"
    "*.inl"
)

add_library(galay-ssl ${SSL_SOURCES})
add_library(galay::ssl ALIAS galay-ssl)

galay_ensure_openssl()

target_include_directories(galay-ssl
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(galay-ssl
    PUBLIC
        galay::kernel
        galay::utils
        OpenSSL::SSL
        OpenSSL::Crypto
)

set_target_properties(galay-ssl PROPERTIES EXPORT_NAME ssl)
