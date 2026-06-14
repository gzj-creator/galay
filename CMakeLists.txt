cmake_minimum_required(VERSION 3.20)

project(galay VERSION 6.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/option.cmake)
include(cmake/dependencies.cmake)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

if(NOT DEFINED BUILD_TESTING)
    set(BUILD_TESTING OFF CACHE BOOL "Build tests" FORCE)
endif()
include(CTest)

if(GALAY_BUILD_UTILS)
    add_subdirectory(src/utils)
endif()
if(GALAY_BUILD_KERNEL)
    add_subdirectory(src/kernel)
endif()
if(GALAY_BUILD_SSL)
    add_subdirectory(src/ssl)
endif()
if(GALAY_BUILD_HTTP)
    add_subdirectory(src/http)
endif()
if(GALAY_BUILD_WS)
    add_subdirectory(src/ws)
endif()
if(GALAY_BUILD_HTTP2)
    add_subdirectory(src/http2)
endif()
if(GALAY_BUILD_REDIS)
    add_subdirectory(src/redis)
endif()
if(GALAY_BUILD_RPC)
    add_subdirectory(src/rpc)
endif()
if(GALAY_BUILD_MYSQL)
    add_subdirectory(src/mysql)
endif()
if(GALAY_BUILD_MONGO)
    add_subdirectory(src/mongo)
endif()
if(GALAY_BUILD_ETCD)
    add_subdirectory(src/etcd)
endif()
if(GALAY_BUILD_MCP)
    add_subdirectory(src/mcp)
endif()
if(GALAY_BUILD_TRACING)
    add_subdirectory(src/tracing)
endif()

set(GALAY_INSTALL_TARGETS)
foreach(_galay_target IN ITEMS
    utils kernel ssl http ws http2 redis rpc mysql mongo etcd mcp tracing
    galay-tracing-spdlog galay-tracing-kernel
)
    if(TARGET ${_galay_target})
        list(APPEND GALAY_INSTALL_TARGETS ${_galay_target})
    endif()
endforeach()

if(GALAY_INSTALL_TARGETS)
    install(TARGETS ${GALAY_INSTALL_TARGETS}
        EXPORT galayTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    )
endif()

install(DIRECTORY ${PROJECT_SOURCE_DIR}/src/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING
        PATTERN "*.h"
        PATTERN "*.hpp"
        PATTERN "*.inl"
        PATTERN "*.cppm"
)

set(GALAY_CONFIG_NEEDS_THREADS ON)
set(GALAY_CONFIG_NEEDS_OPENSSL ${GALAY_BUILD_SSL})

configure_package_config_file(
    ${PROJECT_SOURCE_DIR}/cmake/galayConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/galayConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/galay
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/galayConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(EXPORT galayTargets
    NAMESPACE galay::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/galay
    FILE galayTargets.cmake
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/galayConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/galayConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/galay
)

foreach(_galay_package IN ITEMS
    galay-utils
    galay-kernel
    galay-ssl
    galay-http
    galay-ws
    galay-websocket
    galay-http2
    galay-redis
    galay-rpc
    galay-mysql
    galay-mongo
    galay-etcd
    galay-mcp
    galay-tracing
)
    configure_package_config_file(
        ${PROJECT_SOURCE_DIR}/cmake/galay-module-config.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/${_galay_package}Config.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${_galay_package}
    )
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/${_galay_package}ConfigVersion.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion
    )
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/${_galay_package}Config.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/${_galay_package}ConfigVersion.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/${_galay_package}
    )
endforeach()

if(BUILD_TESTING)
    add_subdirectory(test)
endif()
if(GALAY_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
if(GALAY_BUILD_BENCHMARKS)
    add_subdirectory(benchmark)
endif()
