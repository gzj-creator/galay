cmake_minimum_required(VERSION 3.20)

project(galay VERSION 6.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/option.cmake)
include(cmake/dependencies.cmake)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

include(CTest)

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/include/galay)
file(REMOVE_RECURSE ${CMAKE_BINARY_DIR}/include/galay/cpp)
file(CREATE_LINK
    ${PROJECT_SOURCE_DIR}/src/cpp
    ${CMAKE_BINARY_DIR}/include/galay/cpp
    SYMBOLIC
    COPY_ON_ERROR
)
if(GALAY_BUILD_C_API)
    file(REMOVE_RECURSE ${CMAKE_BINARY_DIR}/include/galay/c)
    file(CREATE_LINK
        ${PROJECT_SOURCE_DIR}/src/c
        ${CMAKE_BINARY_DIR}/include/galay/c
        SYMBOLIC
        COPY_ON_ERROR
    )
endif()

if(GALAY_BUILD_UTILS)
    add_subdirectory(src/cpp/galay-utils)
endif()
if(GALAY_BUILD_KERNEL)
    add_subdirectory(src/cpp/galay-kernel)
endif()
if(GALAY_BUILD_SSL)
    add_subdirectory(src/cpp/galay-ssl)
endif()
if(GALAY_BUILD_HTTP)
    add_subdirectory(src/cpp/galay-http)
endif()
if(GALAY_BUILD_WS)
    add_subdirectory(src/cpp/galay-ws)
endif()
if(GALAY_BUILD_HTTP2)
    add_subdirectory(src/cpp/galay-http2)
endif()
if(GALAY_BUILD_REDIS)
    add_subdirectory(src/cpp/galay-redis)
endif()
if(GALAY_BUILD_RPC)
    add_subdirectory(src/cpp/galay-rpc)
endif()
if(GALAY_BUILD_MYSQL)
    add_subdirectory(src/cpp/galay-mysql)
endif()
if(GALAY_BUILD_MONGO)
    add_subdirectory(src/cpp/galay-mongo)
endif()
if(GALAY_BUILD_ETCD)
    add_subdirectory(src/cpp/galay-etcd)
endif()
if(GALAY_BUILD_MCP)
    add_subdirectory(src/cpp/galay-mcp)
endif()
if(GALAY_BUILD_TRACING)
    add_subdirectory(src/cpp/galay-tracing)
endif()
if(GALAY_BUILD_C_API)
    add_subdirectory(src/c)
endif()

set(GALAY_INSTALL_TARGETS)
foreach(_galay_target IN ITEMS
    utils kernel ssl http ws http2 redis rpc mysql mongo etcd mcp tracing
    galay-tracing-spdlog galay-tracing-kernel
    galay-c-common
    galay-c-utils galay-c-kernel galay-c-ssl galay-c-http galay-c-ws
    galay-c-http2 galay-c-redis galay-c-rpc galay-c-mysql
    galay-c-mongo galay-c-etcd galay-c-mcp galay-c-tracing
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
    )
endif()

install(DIRECTORY ${PROJECT_SOURCE_DIR}/src/cpp/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/galay/cpp
    FILES_MATCHING
        PATTERN "*.h"
        PATTERN "*.hpp"
        PATTERN "*.inl"
        PATTERN "*.cppm"
)
if(GALAY_BUILD_C_API)
    install(DIRECTORY ${PROJECT_SOURCE_DIR}/src/c/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/galay/c
        FILES_MATCHING
            PATTERN "*.h"
    )
endif()

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
