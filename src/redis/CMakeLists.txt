if(TARGET galay::kernel)
    set(_galay_redis_kernel_target galay::kernel)
elseif(TARGET galay-kernel)
    add_library(galay::kernel ALIAS galay-kernel)
    set(_galay_redis_kernel_target galay::kernel)
elseif(TARGET kernel)
    add_library(galay::kernel ALIAS kernel)
    set(_galay_redis_kernel_target galay::kernel)
elseif(TARGET galay-kernel::galay-kernel)
    set(_galay_redis_kernel_target galay-kernel::galay-kernel)
else()
    message(FATAL_ERROR "redis requires an in-tree kernel target")
endif()

if(TARGET galay::utils)
    set(_galay_redis_utils_target galay::utils)
elseif(TARGET galay-utils)
    add_library(galay::utils ALIAS galay-utils)
    set(_galay_redis_utils_target galay::utils)
elseif(TARGET utils)
    add_library(galay::utils ALIAS utils)
    set(_galay_redis_utils_target galay::utils)
elseif(TARGET galay::galay-utils)
    set(_galay_redis_utils_target galay::galay-utils)
else()
    message(FATAL_ERROR "redis requires an in-tree utils target")
endif()

if(GALAY_SSL_FEATURE_ENABLED)
    if(TARGET galay::ssl)
        set(_galay_redis_ssl_target galay::ssl)
    elseif(TARGET galay-ssl)
        add_library(galay::ssl ALIAS galay-ssl)
        set(_galay_redis_ssl_target galay::ssl)
    elseif(TARGET ssl)
        add_library(galay::ssl ALIAS ssl)
        set(_galay_redis_ssl_target galay::ssl)
    else()
        message(FATAL_ERROR "redis requires an in-tree ssl target when GALAY_SSL_FEATURE_ENABLED is ON")
    endif()
endif()

file(GLOB GALAY_REDIS_BASE_SOURCES CONFIGURE_DEPENDS "base/*.cc")
file(GLOB GALAY_REDIS_ASYNC_SOURCES CONFIGURE_DEPENDS "async/*.cc")
file(GLOB GALAY_REDIS_PROTOCOL_SOURCES CONFIGURE_DEPENDS "protoc/*.cc")
file(GLOB GALAY_REDIS_SYNC_SOURCES CONFIGURE_DEPENDS "sync/*.cc")

set(GALAY_REDIS_SOURCES
    ${GALAY_REDIS_BASE_SOURCES}
    ${GALAY_REDIS_ASYNC_SOURCES}
    ${GALAY_REDIS_PROTOCOL_SOURCES}
    ${GALAY_REDIS_SYNC_SOURCES}
)

if(NOT GALAY_SSL_FEATURE_ENABLED)
    list(REMOVE_ITEM GALAY_REDIS_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/async/client_tls.cc")
endif()

add_library(redis ${GALAY_REDIS_SOURCES})
add_library(galay::redis ALIAS redis)

if(NOT TARGET galay-redis)
    add_library(galay-redis ALIAS redis)
endif()
if(NOT TARGET galay-redis::galay-redis)
    add_library(galay-redis::galay-redis ALIAS redis)
endif()

target_compile_features(redis PUBLIC cxx_std_23)
target_include_directories(redis
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(redis
    PUBLIC
        ${_galay_redis_kernel_target}
        ${_galay_redis_utils_target}
)

if(GALAY_SSL_FEATURE_ENABLED)
    target_link_libraries(redis PUBLIC ${_galay_redis_ssl_target})
    target_compile_definitions(redis PUBLIC GALAY_SSL_FEATURE_ENABLED)
endif()

set_target_properties(redis PROPERTIES EXPORT_NAME redis)
