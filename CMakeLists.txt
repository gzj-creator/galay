cmake_minimum_required(VERSION 3.20)

project(galay VERSION 4.9.3 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/option.cmake)
include(cmake/dependencies.cmake)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Keep an unbounded test from stalling a normal ctest run.  Individual tests
# with a legitimate longer budget set their own TIMEOUT property.
set(DART_TESTING_TIMEOUT "${GALAY_CTEST_DEFAULT_TIMEOUT}" CACHE STRING
    "Maximum time allowed before CTest kills a test" FORCE)
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
if(GALAY_BUILD_ETCD)
    add_subdirectory(src/cpp/galay-etcd)
endif()
if(GALAY_BUILD_MYSQL)
    add_subdirectory(src/cpp/galay-mysql)
endif()
if(GALAY_BUILD_POSTGRES)
    add_subdirectory(src/cpp/galay-postgres)
endif()
if(GALAY_BUILD_MONGO)
    add_subdirectory(src/cpp/galay-mongo)
endif()
if(GALAY_BUILD_RPC)
    add_subdirectory(src/cpp/galay-rpc)
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
    galay-utils galay-kernel galay-ssl galay-http galay-ws galay-http2
    galay-redis galay-rpc galay-mysql galay-postgres galay-mongo galay-etcd galay-mcp
    galay-tracing
    galay-tracing-spdlog galay-tracing-kernel
    galay-c-common
    galay-c-utils galay-c-kernel galay-c-ssl galay-c-http galay-c-ws
    galay-c-http2 galay-c-redis galay-c-rpc galay-c-mysql galay-c-postgres
    galay-c-mongo galay-c-etcd galay-c-mcp galay-c-tracing
)
    if(TARGET ${_galay_target})
        list(APPEND GALAY_INSTALL_TARGETS ${_galay_target})
    endif()
endforeach()

if(GALAY_KERNEL_CPP23_MODULES_EFFECTIVE)
    list(REMOVE_ITEM GALAY_INSTALL_TARGETS galay-kernel)
endif()
if(GALAY_RPC_CPP23_MODULES_EFFECTIVE)
    list(REMOVE_ITEM GALAY_INSTALL_TARGETS galay-rpc)
endif()
if(GALAY_POSTGRES_CPP23_MODULES_EFFECTIVE)
    list(REMOVE_ITEM GALAY_INSTALL_TARGETS galay-postgres)
endif()

if(GALAY_INSTALL_TARGETS)
    install(TARGETS ${GALAY_INSTALL_TARGETS}
        EXPORT galayTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

# Keep the native module target installation in one place.  Build-only module
# targets are not exported when interface installation is disabled, so this
# branch needs no FILE_SET argument.
function(galay_install_native_cpp23_module_target target module_dir)
    if(GALAY_INSTALL_CPP23_MODULE_INTERFACES)
        install(TARGETS ${target}
            EXPORT galayTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
            FILE_SET CXX_MODULES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/galay/cpp/${module_dir}/module
            CXX_MODULES_BMI DESTINATION ${CMAKE_INSTALL_LIBDIR}/galay/cpp/modules
        )
        install(FILES ${PROJECT_SOURCE_DIR}/src/cpp/${module_dir}/module/module_prelude.hpp
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/galay/cpp/${module_dir}/module
        )
    else()
        install(TARGETS ${target}
            EXPORT galayTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
            RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        )
    endif()
endfunction()

if(TARGET galay-kernel AND GALAY_KERNEL_CPP23_MODULES_EFFECTIVE)
    galay_install_native_cpp23_module_target(galay-kernel galay-kernel)
endif()

if(TARGET galay-rpc AND GALAY_RPC_CPP23_MODULES_EFFECTIVE)
    galay_install_native_cpp23_module_target(galay-rpc galay-rpc)
endif()

if(TARGET galay-postgres AND GALAY_POSTGRES_CPP23_MODULES_EFFECTIVE)
    galay_install_native_cpp23_module_target(galay-postgres galay-postgres)
endif()

# A CMake generator may be unable to build C++ module file sets while mcpp can
# still consume the source interfaces directly.  Keep interface installation
# independent from the native CMake module dependency-scanning capability.
function(galay_install_cpp23_module_interface build_option module_dir module_file effective)
    if("${${build_option}}" AND NOT "${effective}")
        install(FILES
            ${PROJECT_SOURCE_DIR}/src/cpp/${module_dir}/module/${module_file}
            ${PROJECT_SOURCE_DIR}/src/cpp/${module_dir}/module/module_prelude.hpp
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/galay/cpp/${module_dir}/module
        )
    endif()
endfunction()

if(GALAY_INSTALL_CPP23_MODULE_INTERFACES)
    galay_install_cpp23_module_interface(
        GALAY_BUILD_UTILS galay-utils galay_utils.cppm "${GALAY_UTILS_CPP23_MODULES_EFFECTIVE}")
    galay_install_cpp23_module_interface(
        GALAY_BUILD_KERNEL galay-kernel galay_kernel.cppm "${GALAY_KERNEL_CPP23_MODULES_EFFECTIVE}")
    galay_install_cpp23_module_interface(
        GALAY_BUILD_SSL galay-ssl galay_ssl.cppm "${GALAY_SSL_CPP23_MODULES_EFFECTIVE}")
    galay_install_cpp23_module_interface(
        GALAY_BUILD_HTTP galay-http galay_http.cppm "${GALAY_HTTP_CPP23_MODULES_EFFECTIVE}")
    galay_install_cpp23_module_interface(
        GALAY_BUILD_WS galay-ws galay_websocket.cppm "${GALAY_WS_CPP23_MODULES_EFFECTIVE}")
    galay_install_cpp23_module_interface(
        GALAY_BUILD_HTTP2 galay-http2 galay_http2.cppm "${GALAY_HTTP2_CPP23_MODULES_EFFECTIVE}")
    galay_install_cpp23_module_interface(
        GALAY_BUILD_REDIS galay-redis galay_redis.cppm "${GALAY_REDIS_CPP23_MODULES_EFFECTIVE}")
    galay_install_cpp23_module_interface(
        GALAY_BUILD_ETCD galay-etcd galay_etcd.cppm "${GALAY_ETCD_CPP23_MODULES_EFFECTIVE}")
    galay_install_cpp23_module_interface(
        GALAY_BUILD_MYSQL galay-mysql galay_mysql.cppm "${GALAY_MYSQL_CPP23_MODULES_EFFECTIVE}")
    galay_install_cpp23_module_interface(
        GALAY_BUILD_POSTGRES galay-postgres galay_postgres.cppm "${GALAY_POSTGRES_CPP23_MODULES_EFFECTIVE}")
    galay_install_cpp23_module_interface(
        GALAY_BUILD_MONGO galay-mongo galay_mongo.cppm "${GALAY_MONGO_CPP23_MODULES_EFFECTIVE}")
    galay_install_cpp23_module_interface(
        GALAY_BUILD_RPC galay-rpc galay_rpc.cppm "${GALAY_RPC_CPP23_MODULES_EFFECTIVE}")
    galay_install_cpp23_module_interface(
        GALAY_BUILD_MCP galay-mcp galay_mcp.cppm "${GALAY_MCP_CPP23_MODULES_EFFECTIVE}")
    galay_install_cpp23_module_interface(
        GALAY_BUILD_TRACING galay-tracing galay_tracing.cppm "${GALAY_TRACING_CPP23_MODULES_EFFECTIVE}")
    if(GALAY_BUILD_RPC AND GALAY_RPC_ENABLE_ETCD AND NOT GALAY_RPC_CPP23_MODULES_EFFECTIVE)
        install(FILES
            ${PROJECT_SOURCE_DIR}/src/cpp/galay-rpc/module/galay_rpc_etcd.cppm
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/galay/cpp/galay-rpc/module
        )
    endif()
endif()

install(DIRECTORY ${PROJECT_SOURCE_DIR}/src/cpp/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/galay/cpp
    FILES_MATCHING
        PATTERN "module" EXCLUDE
        PATTERN "*.h"
        PATTERN "*.hpp"
        PATTERN "*.inl"
)
if(GALAY_BUILD_C_API)
    install(DIRECTORY ${PROJECT_SOURCE_DIR}/src/c/
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/galay/c
        FILES_MATCHING
            PATTERN "*.h"
    )
endif()

set(GALAY_CONFIG_NEEDS_THREADS ON)
set(GALAY_CONFIG_NEEDS_OPENSSL OFF)
if(TARGET galay-ssl OR TARGET galay-tracing)
    set(GALAY_CONFIG_NEEDS_OPENSSL ON)
endif()
set(GALAY_CONFIG_NEEDS_SPDLOG OFF)
if(TARGET galay-tracing-spdlog)
    set(GALAY_CONFIG_NEEDS_SPDLOG ON)
endif()

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

if(GALAY_INSTALL_CPP23_MODULE_INTERFACES AND
   (GALAY_KERNEL_CPP23_MODULES_EFFECTIVE OR
    GALAY_RPC_CPP23_MODULES_EFFECTIVE OR
    GALAY_POSTGRES_CPP23_MODULES_EFFECTIVE))
    install(EXPORT galayTargets
        NAMESPACE galay::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/galay
        FILE galayTargets.cmake
        CXX_MODULES_DIRECTORY cxx-modules
    )
else()
    install(EXPORT galayTargets
        NAMESPACE galay::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/galay
        FILE galayTargets.cmake
    )
endif()
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/galayConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/galayConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/galay
)

if(BUILD_TESTING)
    add_subdirectory(test)
endif()
if(GALAY_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()
if(GALAY_BUILD_BENCHMARKS)
    add_subdirectory(benchmark)
endif()
