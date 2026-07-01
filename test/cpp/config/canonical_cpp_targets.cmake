cmake_minimum_required(VERSION 3.20)

foreach(required_var
        IN ITEMS
        GALAY_SOURCE_DIR
        GALAY_BINARY_DIR
        GALAY_CMAKE_GENERATOR
        GALAY_CXX_COMPILER)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "canonical_cpp_targets requires `${required_var}`.")
    endif()
endforeach()

set(canonical_modules
    utils
    kernel
    ssl
    http
    ws
    http2
    redis
    rpc
    mysql
    mongo
    etcd
    mcp
    tracing
)

foreach(module IN LISTS canonical_modules)
    set(module_cmake "${GALAY_SOURCE_DIR}/src/cpp/galay-${module}/CMakeLists.txt")
    if(NOT EXISTS "${module_cmake}")
        message(FATAL_ERROR "Missing CMake module file: ${module_cmake}")
    endif()

    file(READ "${module_cmake}" module_cmake_content)
    if(NOT module_cmake_content MATCHES "add_library\\(galay-${module}([ \t\r\n]|\\))")
        message(FATAL_ERROR "Module ${module} must create canonical target galay-${module}.")
    endif()
    if(NOT module_cmake_content MATCHES "add_library\\(galay::${module}[ \t\r\n]+ALIAS[ \t\r\n]+galay-${module}\\)")
        message(FATAL_ERROR "Module ${module} must expose only canonical alias galay::${module}.")
    endif()
    if(module_cmake_content MATCHES "add_library\\(${module}([ \t\r\n]|\\))")
        message(FATAL_ERROR "Module ${module} must not create the old bare target `${module}`.")
    endif()
    if(module_cmake_content MATCHES "add_library\\(galay-${module}::galay-${module}[ \t\r\n]+ALIAS")
        message(FATAL_ERROR "Module ${module} must not keep the old package namespace alias.")
    endif()
    if(module_cmake_content MATCHES "add_library\\(galay::galay-${module}[ \t\r\n]+ALIAS")
        message(FATAL_ERROR "Module ${module} must not keep a duplicated galay::galay-${module} alias.")
    endif()
    if(module_cmake_content MATCHES "galay-${module}-modules|galay::${module}-modules")
        message(FATAL_ERROR "Module ${module} C++ module support must attach to galay-${module}, not a -modules target.")
    endif()
endforeach()

file(GLOB module_cmake_files "${GALAY_SOURCE_DIR}/src/cpp/galay-*/CMakeLists.txt")
foreach(module_cmake IN LISTS module_cmake_files)
    file(READ "${module_cmake}" module_cmake_content)
    if(module_cmake_content MATCHES "elseif\\(TARGET[ \t\r\n]+(kernel|utils|http|ssl)\\)")
        message(FATAL_ERROR "Legacy bare-target fallback remains in ${module_cmake}.")
    endif()
    if(module_cmake_content MATCHES "elseif\\(TARGET[ \t\r\n]+galay-(kernel|utils|http|ssl)::galay-(kernel|utils|http|ssl)\\)")
        message(FATAL_ERROR "Legacy package-namespace fallback remains in ${module_cmake}.")
    endif()
endforeach()

file(READ "${GALAY_SOURCE_DIR}/src/cpp/galay-rpc/CMakeLists.txt" rpc_cmake_content)
if(rpc_cmake_content MATCHES "galay-rpc-modules|galay::rpc-modules")
    message(FATAL_ERROR "RPC C++ module support must use the canonical galay-rpc target, not a -modules facade target.")
endif()
if(rpc_cmake_content MATCHES "galay-rpc-etcd|galay::rpc-etcd")
    message(FATAL_ERROR "RPC etcd integration must be compiled into the canonical galay-rpc target, not a separate rpc-etcd target.")
endif()
if(NOT rpc_cmake_content MATCHES "target_sources\\(galay-rpc")
    message(FATAL_ERROR "RPC C++ module sources must be attached to the canonical galay-rpc target.")
endif()

file(READ "${GALAY_SOURCE_DIR}/CMakeLists.txt" root_cmake_content)
if(root_cmake_content MATCHES "galay-rpc-etcd|galay::rpc-etcd|galay-rpc-modules|galay::rpc-modules")
    message(FATAL_ERROR "Root CMakeLists must not install or export separate RPC module or rpc-etcd targets.")
endif()

file(GLOB_RECURSE consumer_cmake_files
    "${GALAY_SOURCE_DIR}/examples/cpp/*/CMakeLists.txt"
    "${GALAY_SOURCE_DIR}/test/cpp/*/CMakeLists.txt"
    "${GALAY_SOURCE_DIR}/benchmark/cpp/*/CMakeLists.txt")
foreach(consumer_cmake IN LISTS consumer_cmake_files)
    file(READ "${consumer_cmake}" consumer_cmake_content)
    if(consumer_cmake_content MATCHES "galay-[A-Za-z0-9_]+-modules|galay::[A-Za-z0-9_]+-modules|galay-[A-Za-z0-9_]+::galay-[A-Za-z0-9_]+-modules")
        message(FATAL_ERROR "C++ module consumers must link canonical galay::<module> targets, not -modules facade targets: ${consumer_cmake}")
    endif()
endforeach()

set(smoke_root "${GALAY_BINARY_DIR}/test/canonical-cpp-targets")
set(consumer_source_dir "${smoke_root}/consumer")
set(consumer_build_dir "${smoke_root}/build")
file(REMOVE_RECURSE "${smoke_root}")
file(MAKE_DIRECTORY "${consumer_source_dir}")

file(WRITE "${consumer_source_dir}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.20)
project(galay_canonical_cpp_targets LANGUAGES C CXX)

set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(GALAY_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GALAY_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(GALAY_BUILD_C_API OFF CACHE BOOL "" FORCE)
set(GALAY_BUILD_SSL OFF CACHE BOOL "" FORCE)
set(GALAY_BUILD_HTTP OFF CACHE BOOL "" FORCE)
set(GALAY_BUILD_WS OFF CACHE BOOL "" FORCE)
set(GALAY_BUILD_HTTP2 OFF CACHE BOOL "" FORCE)
set(GALAY_BUILD_REDIS OFF CACHE BOOL "" FORCE)
set(GALAY_BUILD_ETCD OFF CACHE BOOL "" FORCE)
set(GALAY_BUILD_MONGO OFF CACHE BOOL "" FORCE)
set(GALAY_BUILD_MYSQL OFF CACHE BOOL "" FORCE)
set(GALAY_BUILD_RPC OFF CACHE BOOL "" FORCE)
set(GALAY_BUILD_MCP OFF CACHE BOOL "" FORCE)
set(GALAY_BUILD_TRACING OFF CACHE BOOL "" FORCE)

add_subdirectory("@GALAY_SOURCE_DIR@" galay)

foreach(required_target IN ITEMS galay-utils galay-kernel galay::utils galay::kernel)
    if(NOT TARGET ${required_target})
        message(FATAL_ERROR "Missing canonical target: ${required_target}")
    endif()
endforeach()

foreach(forbidden_target IN ITEMS utils kernel galay::galay-utils galay-kernel::galay-kernel)
    if(TARGET ${forbidden_target})
        message(FATAL_ERROR "Unexpected legacy target: ${forbidden_target}")
    endif()
endforeach()
]=])

file(READ "${consumer_source_dir}/CMakeLists.txt" consumer_cmake_content)
string(REPLACE "@GALAY_SOURCE_DIR@" "${GALAY_SOURCE_DIR}" consumer_cmake_content "${consumer_cmake_content}")
file(WRITE "${consumer_source_dir}/CMakeLists.txt" "${consumer_cmake_content}")

set(configure_command
    "${CMAKE_COMMAND}"
    -S "${consumer_source_dir}"
    -B "${consumer_build_dir}"
    -G "${GALAY_CMAKE_GENERATOR}"
    "-DCMAKE_CXX_COMPILER=${GALAY_CXX_COMPILER}")

execute_process(
    COMMAND ${configure_command}
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_stdout
    ERROR_VARIABLE configure_stderr)

if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR
        "Failed to configure canonical C++ target smoke project.\n"
        "stdout:\n${configure_stdout}\n"
        "stderr:\n${configure_stderr}")
endif()
