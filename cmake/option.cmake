include_guard(GLOBAL)

# 是否使用 Debug 配置构建；关闭时默认使用 Release 配置。
option(GALAY_BUILD_DEBUG "Build with Debug configuration instead of Release" OFF)

if(CMAKE_CONFIGURATION_TYPES)
    if(GALAY_BUILD_DEBUG)
        set(CMAKE_DEFAULT_BUILD_TYPE "Debug" CACHE STRING "Default build configuration" FORCE)
    else()
        set(CMAKE_DEFAULT_BUILD_TYPE "Release" CACHE STRING "Default build configuration" FORCE)
    endif()
elseif(GALAY_BUILD_DEBUG)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
elseif(NOT CMAKE_BUILD_TYPE OR CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

# 是否构建 utils C++ 模块。
option(GALAY_BUILD_UTILS "Build the utils module" ON)
# 是否构建 kernel C++ 模块。
option(GALAY_BUILD_KERNEL "Build the kernel module" ON)
# 是否构建 SSL C++ 模块。
option(GALAY_BUILD_SSL "Build the ssl module" ON)
# 是否构建 HTTP C++ 模块。
option(GALAY_BUILD_HTTP "Build the http module" ON)
# 是否构建 WebSocket C++ 模块。
option(GALAY_BUILD_WS "Build the websocket module" ON)
# 是否构建 HTTP/2 C++ 模块。
option(GALAY_BUILD_HTTP2 "Build the http2 module" ON)
# 是否构建 Redis C++ 模块。
option(GALAY_BUILD_REDIS "Build the redis module" ON)
# 是否构建 etcd C++ 模块。
option(GALAY_BUILD_ETCD "Build the etcd module" ON)
# 是否构建 MongoDB C++ 模块。
option(GALAY_BUILD_MONGO "Build the mongo module" ON)
# 是否构建 MySQL C++ 模块。
option(GALAY_BUILD_MYSQL "Build the mysql module" ON)
# 是否构建 PostgreSQL C++ 模块。
option(GALAY_BUILD_POSTGRES "Build the postgres module" ON)
# 是否构建 RPC C++ 模块。
option(GALAY_BUILD_RPC "Build the rpc module" ON)
# 是否构建 MCP C++ 模块。
option(GALAY_BUILD_MCP "Build the mcp module" ON)
# 是否构建 tracing C++ 模块。
option(GALAY_BUILD_TRACING "Build the tracing module" ON)

# 是否构建并注册 CTest 测试。
option(BUILD_TESTING "Build tests" OFF)
set(GALAY_CTEST_DEFAULT_TIMEOUT 30 CACHE STRING
    "Default timeout in seconds for CTest tests without an explicit TIMEOUT property")
if(NOT GALAY_CTEST_DEFAULT_TIMEOUT MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "GALAY_CTEST_DEFAULT_TIMEOUT must be a positive whole number of seconds")
endif()
# 是否构建已启用模块的示例程序。
option(GALAY_BUILD_EXAMPLES "Build enabled module examples" OFF)
# 是否构建已启用模块的 benchmark 程序。
option(GALAY_BUILD_BENCHMARKS "Build enabled module benchmarks" OFF)
# 是否构建 C ABI 包装目标。
option(GALAY_BUILD_C_API "Build C ABI wrapper targets" ON)

# 当 CMake、生成器和编译器支持命名模块依赖扫描时，
# 是否启用 CMake 原生 C++23 模块文件集；只影响模块扫描与编译。
option(GALAY_ENABLE_CPP23_MODULES "Build enabled module C++23 facade targets when supported" OFF)

# 是否安装供 mcpp 等外部模块工具使用的 .cppm 接口和 module_prelude.hpp；
# 与 CMake 原生模块扫描和编译相互独立。
option(GALAY_INSTALL_CPP23_MODULE_INTERFACES
    "Install C++23 module interfaces for external module-aware build tools" ON)

# 是否将非纯头文件模块构建为共享库；关闭时构建静态库。
option(GALAY_BUILD_SHARED_LIBS "Build non-header modules as shared libraries" ON)
# 是否禁用 Linux io_uring；关闭时在可用环境中优先使用 io_uring。
option(GALAY_DISABLE_IOURING "Disable io_uring and use epoll on Linux" ON)

# 是否构建 tracing 的 spdlog 适配器。
option(GALAY_TRACING_ENABLE_SPDLOG "Enable the tracing spdlog adapter" OFF)
# 是否启用 tracing 内置的 galay-http OTLP 传输适配器。
option(GALAY_TRACING_ENABLE_GALAY_HTTP_OTLP_TRANSPORT "Enable the built-in galay-http OTLP transport" OFF)
# 是否将 etcd 服务发现适配器编译进 galay::rpc。
option(GALAY_RPC_ENABLE_ETCD "Compile the RPC etcd discovery adapter into galay::rpc" OFF)

if(DEFINED GALAY_TRACING_ENABLE_OTLP_HTTP)
    message(DEPRECATION
        "GALAY_TRACING_ENABLE_OTLP_HTTP is deprecated; use "
        "GALAY_TRACING_ENABLE_GALAY_HTTP_OTLP_TRANSPORT.")
    set(GALAY_TRACING_ENABLE_GALAY_HTTP_OTLP_TRANSPORT
        "${GALAY_TRACING_ENABLE_OTLP_HTTP}"
        CACHE BOOL "Enable the built-in galay-http OTLP transport" FORCE)
endif()

set(BUILD_SHARED_LIBS "${GALAY_BUILD_SHARED_LIBS}" CACHE BOOL "Build shared libraries" FORCE)
set(ENABLE_CPP23_MODULES "${GALAY_ENABLE_CPP23_MODULES}" CACHE BOOL "Enable C++23 module targets" FORCE)

if(GALAY_BUILD_KERNEL AND NOT GALAY_BUILD_UTILS)
    message(FATAL_ERROR "GALAY_BUILD_KERNEL requires GALAY_BUILD_UTILS")
endif()
if(GALAY_BUILD_SSL AND NOT GALAY_BUILD_KERNEL)
    message(FATAL_ERROR "GALAY_BUILD_SSL requires GALAY_BUILD_KERNEL")
endif()
if(GALAY_BUILD_HTTP AND NOT GALAY_BUILD_KERNEL)
    message(FATAL_ERROR "GALAY_BUILD_HTTP requires GALAY_BUILD_KERNEL")
endif()
if(GALAY_BUILD_WS AND NOT GALAY_BUILD_HTTP)
    message(FATAL_ERROR "GALAY_BUILD_WS requires GALAY_BUILD_HTTP")
endif()
if(GALAY_BUILD_HTTP2 AND NOT GALAY_BUILD_HTTP)
    message(FATAL_ERROR "GALAY_BUILD_HTTP2 requires GALAY_BUILD_HTTP")
endif()
if(GALAY_BUILD_POSTGRES AND NOT GALAY_BUILD_KERNEL)
    message(FATAL_ERROR "GALAY_BUILD_POSTGRES requires GALAY_BUILD_KERNEL")
endif()
if(GALAY_RPC_ENABLE_ETCD AND NOT GALAY_BUILD_RPC)
    message(FATAL_ERROR "GALAY_RPC_ENABLE_ETCD requires GALAY_BUILD_RPC")
endif()
if(GALAY_RPC_ENABLE_ETCD AND NOT GALAY_BUILD_ETCD)
    message(FATAL_ERROR "GALAY_RPC_ENABLE_ETCD requires GALAY_BUILD_ETCD")
endif()

if(GALAY_BUILD_SSL)
    set(GALAY_SSL_FEATURE_ENABLED ON)
else()
    set(GALAY_SSL_FEATURE_ENABLED OFF)
endif()
