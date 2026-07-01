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

add_library(galay-redis ${GALAY_REDIS_SOURCES})
add_library(galay::redis ALIAS galay-redis)

target_compile_features(galay-redis PUBLIC cxx_std_23)
target_include_directories(galay-redis
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(galay-redis
    PUBLIC
        galay::kernel
        galay::utils
)

if(GALAY_SSL_FEATURE_ENABLED)
    target_link_libraries(galay-redis PUBLIC galay::ssl)
    target_compile_definitions(galay-redis PUBLIC GALAY_SSL_FEATURE_ENABLED)
endif()

set_target_properties(galay-redis PROPERTIES EXPORT_NAME redis)
