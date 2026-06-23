add_subdirectory(galay-c)

if(GALAY_BUILD_UTILS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/galay-utils/CMakeLists.txt")
    add_subdirectory(galay-utils)
endif()
if(GALAY_BUILD_KERNEL AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/galay-kernel/CMakeLists.txt")
    add_subdirectory(galay-kernel)
endif()
if(GALAY_BUILD_SSL AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/galay-ssl/CMakeLists.txt")
    add_subdirectory(galay-ssl)
endif()
if(GALAY_BUILD_HTTP AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/galay-http/CMakeLists.txt")
    add_subdirectory(galay-http)
endif()
if(GALAY_BUILD_WS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/galay-ws/CMakeLists.txt")
    add_subdirectory(galay-ws)
endif()
if(GALAY_BUILD_HTTP2 AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/galay-http2/CMakeLists.txt")
    add_subdirectory(galay-http2)
endif()
if(GALAY_BUILD_REDIS AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/galay-redis/CMakeLists.txt")
    add_subdirectory(galay-redis)
endif()
if(GALAY_BUILD_RPC AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/galay-rpc/CMakeLists.txt")
    add_subdirectory(galay-rpc)
endif()
if(GALAY_BUILD_MYSQL AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/galay-mysql/CMakeLists.txt")
    add_subdirectory(galay-mysql)
endif()
if(GALAY_BUILD_MONGO AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/galay-mongo/CMakeLists.txt")
    add_subdirectory(galay-mongo)
endif()
if(GALAY_BUILD_ETCD AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/galay-etcd/CMakeLists.txt")
    add_subdirectory(galay-etcd)
endif()
if(GALAY_BUILD_MCP AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/galay-mcp/CMakeLists.txt")
    add_subdirectory(galay-mcp)
endif()
if(GALAY_BUILD_TRACING AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/galay-tracing/CMakeLists.txt")
    add_subdirectory(galay-tracing)
endif()

function(galay_configure_c_api_target target_name)
    if(NOT TARGET "${target_name}")
        return()
    endif()

    get_target_property(_galay_c_target_type "${target_name}" TYPE)
    if(_galay_c_target_type STREQUAL "SHARED_LIBRARY" OR
       _galay_c_target_type STREQUAL "MODULE_LIBRARY")
        target_compile_definitions("${target_name}"
            PUBLIC GALAY_C_SHARED
            PRIVATE GALAY_C_BUILDING
        )
        set_target_properties("${target_name}" PROPERTIES
            C_VISIBILITY_PRESET hidden
            CXX_VISIBILITY_PRESET hidden
            VISIBILITY_INLINES_HIDDEN ON
            VERSION ${PROJECT_VERSION}
            SOVERSION ${PROJECT_VERSION_MAJOR}
        )
    endif()
endfunction()

foreach(_galay_c_target IN ITEMS
    galay-c-common
    galay-c-utils
    galay-c-kernel
    galay-c-ssl
    galay-c-http
    galay-c-ws
    galay-c-http2
    galay-c-redis
    galay-c-rpc
    galay-c-mysql
    galay-c-mongo
    galay-c-etcd
    galay-c-mcp
    galay-c-tracing
)
    galay_configure_c_api_target("${_galay_c_target}")
endforeach()
