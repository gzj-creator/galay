file(GLOB_RECURSE SSL_SOURCES CONFIGURE_DEPENDS
    "*.cc"
    "*.h"
    "*.hpp"
    "*.inl"
)

add_library(ssl ${SSL_SOURCES})
add_library(galay::ssl ALIAS ssl)
add_library(galay-ssl::galay-ssl ALIAS ssl)

galay_ensure_openssl()

target_include_directories(ssl
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(ssl
    PUBLIC
        galay::kernel
        galay::utils
        OpenSSL::SSL
        OpenSSL::Crypto
)

set_target_properties(ssl PROPERTIES EXPORT_NAME ssl)
