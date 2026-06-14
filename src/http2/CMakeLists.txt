if(TARGET galay::http)
    set(_galay_http2_http_target galay::http)
elseif(TARGET http)
    add_library(galay::http ALIAS http)
    set(_galay_http2_http_target galay::http)
elseif(TARGET galay-http)
    add_library(galay::http ALIAS galay-http)
    set(_galay_http2_http_target galay::http)
elseif(TARGET galay-http::galay-http)
    set(_galay_http2_http_target galay-http::galay-http)
else()
    message(FATAL_ERROR "http2 requires an in-tree http target")
endif()

if(TARGET galay::kernel)
    set(_galay_http2_kernel_target galay::kernel)
elseif(TARGET galay-kernel)
    add_library(galay::kernel ALIAS galay-kernel)
    set(_galay_http2_kernel_target galay::kernel)
elseif(TARGET kernel)
    add_library(galay::kernel ALIAS kernel)
    set(_galay_http2_kernel_target galay::kernel)
elseif(TARGET galay-kernel::galay-kernel)
    set(_galay_http2_kernel_target galay-kernel::galay-kernel)
else()
    message(FATAL_ERROR "http2 requires an in-tree kernel target")
endif()

if(TARGET galay::utils)
    set(_galay_http2_utils_target galay::utils)
elseif(TARGET galay-utils)
    add_library(galay::utils ALIAS galay-utils)
    set(_galay_http2_utils_target galay::utils)
elseif(TARGET utils)
    add_library(galay::utils ALIAS utils)
    set(_galay_http2_utils_target galay::utils)
elseif(TARGET galay::galay-utils)
    set(_galay_http2_utils_target galay::galay-utils)
else()
    message(FATAL_ERROR "http2 requires an in-tree utils target")
endif()

if(GALAY_SSL_FEATURE_ENABLED)
    if(TARGET galay::ssl)
        set(_galay_http2_ssl_target galay::ssl)
    elseif(TARGET galay-ssl)
        add_library(galay::ssl ALIAS galay-ssl)
        set(_galay_http2_ssl_target galay::ssl)
    elseif(TARGET ssl)
        add_library(galay::ssl ALIAS ssl)
        set(_galay_http2_ssl_target galay::ssl)
    else()
        message(FATAL_ERROR "http2 requires an in-tree ssl target when GALAY_SSL_FEATURE_ENABLED is ON")
    endif()
endif()

file(GLOB_RECURSE GALAY_HTTP2_SOURCES CONFIGURE_DEPENDS "*.cc")

add_library(http2 ${GALAY_HTTP2_SOURCES})
add_library(galay::http2 ALIAS http2)

if(NOT TARGET galay-http2)
    add_library(galay-http2 ALIAS http2)
endif()
if(NOT TARGET galay-http2::galay-http2)
    add_library(galay-http2::galay-http2 ALIAS http2)
endif()

target_compile_features(http2 PUBLIC cxx_std_23)
target_include_directories(http2
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(http2
    PUBLIC
        ${_galay_http2_http_target}
        ${_galay_http2_kernel_target}
        ${_galay_http2_utils_target}
)

if(GALAY_SSL_FEATURE_ENABLED)
    target_link_libraries(http2 PUBLIC ${_galay_http2_ssl_target})
    target_compile_definitions(http2 PUBLIC GALAY_SSL_FEATURE_ENABLED)
endif()

set_target_properties(http2 PROPERTIES EXPORT_NAME http2)
