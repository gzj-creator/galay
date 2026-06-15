if(TARGET galay::http)
    set(_galay_ws_http_target galay::http)
elseif(TARGET http)
    add_library(galay::http ALIAS http)
    set(_galay_ws_http_target galay::http)
elseif(TARGET galay-http)
    add_library(galay::http ALIAS galay-http)
    set(_galay_ws_http_target galay::http)
elseif(TARGET galay-http::galay-http)
    set(_galay_ws_http_target galay-http::galay-http)
else()
    message(FATAL_ERROR "ws requires an in-tree http target")
endif()

if(TARGET galay::kernel)
    set(_galay_ws_kernel_target galay::kernel)
elseif(TARGET galay-kernel)
    add_library(galay::kernel ALIAS galay-kernel)
    set(_galay_ws_kernel_target galay::kernel)
elseif(TARGET kernel)
    add_library(galay::kernel ALIAS kernel)
    set(_galay_ws_kernel_target galay::kernel)
elseif(TARGET galay-kernel::galay-kernel)
    set(_galay_ws_kernel_target galay-kernel::galay-kernel)
else()
    message(FATAL_ERROR "ws requires an in-tree kernel target")
endif()

if(TARGET galay::utils)
    set(_galay_ws_utils_target galay::utils)
elseif(TARGET galay-utils)
    add_library(galay::utils ALIAS galay-utils)
    set(_galay_ws_utils_target galay::utils)
elseif(TARGET utils)
    add_library(galay::utils ALIAS utils)
    set(_galay_ws_utils_target galay::utils)
elseif(TARGET galay::galay-utils)
    set(_galay_ws_utils_target galay::galay-utils)
else()
    message(FATAL_ERROR "ws requires an in-tree utils target")
endif()

if(GALAY_SSL_FEATURE_ENABLED)
    if(TARGET galay::ssl)
        set(_galay_ws_ssl_target galay::ssl)
    elseif(TARGET galay-ssl)
        add_library(galay::ssl ALIAS galay-ssl)
        set(_galay_ws_ssl_target galay::ssl)
    elseif(TARGET ssl)
        add_library(galay::ssl ALIAS ssl)
        set(_galay_ws_ssl_target galay::ssl)
    else()
        message(FATAL_ERROR "ws requires an in-tree ssl target when GALAY_SSL_FEATURE_ENABLED is ON")
    endif()
endif()

file(GLOB_RECURSE GALAY_WS_SOURCES CONFIGURE_DEPENDS "*.cc")

add_library(ws ${GALAY_WS_SOURCES})
add_library(galay::ws ALIAS ws)

if(NOT TARGET galay-ws)
    add_library(galay-ws ALIAS ws)
endif()
if(NOT TARGET galay-websocket)
    add_library(galay-websocket ALIAS ws)
endif()
if(NOT TARGET galay-websocket::galay-websocket)
    add_library(galay-websocket::galay-websocket ALIAS ws)
endif()

target_compile_features(ws PUBLIC cxx_std_23)
target_include_directories(ws
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(ws
    PUBLIC
        ${_galay_ws_http_target}
        ${_galay_ws_kernel_target}
        ${_galay_ws_utils_target}
)

if(GALAY_SSL_FEATURE_ENABLED)
    target_link_libraries(ws PUBLIC ${_galay_ws_ssl_target})
    target_compile_definitions(ws PUBLIC GALAY_SSL_FEATURE_ENABLED)
endif()

set_target_properties(ws PROPERTIES EXPORT_NAME ws)
