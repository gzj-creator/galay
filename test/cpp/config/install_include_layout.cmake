cmake_minimum_required(VERSION 3.20)

foreach(required_var
        IN ITEMS
        GALAY_SOURCE_DIR
        GALAY_BINARY_DIR
        GALAY_CMAKE_GENERATOR
        GALAY_CXX_COMPILER)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "install_include_layout requires `${required_var}`.")
    endif()
endforeach()

set(smoke_root "${GALAY_BINARY_DIR}/test/install-include-layout")
set(prefix_dir "${smoke_root}/prefix")
set(consumer_source_dir "${smoke_root}/consumer")
set(consumer_build_dir "${smoke_root}/consumer-build")

foreach(required_source_path
        IN ITEMS
        "src/cpp/galay-kernel/core/runtime.h"
        "src/cpp/galay-kernel/core/task.h"
        "src/cpp/galay-kernel/common/error.h"
        "src/cpp/galay-kernel/concurrency/spsc/bounded_channel.h"
        "src/cpp/galay-kernel/concurrency/spsc/unbounded_channel.h"
        "src/c/galay-kernel-c/concurrency-c/mpmc/bounded_channel_c.h"
        "src/c/galay-kernel-c/concurrency-c/mpmc/unbounded_channel_c.h"
        "src/c/galay-kernel-c/concurrency-c/mpsc/bounded_channel_c.h"
        "src/c/galay-kernel-c/concurrency-c/mpsc/unbounded_channel_c.h"
        "src/c/galay-kernel-c/concurrency-c/spsc/bounded_channel_c.h"
        "src/c/galay-kernel-c/concurrency-c/spsc/unbounded_channel_c.h"
        "src/cpp/galay-utils/cache/ring_buffer.hpp"
        "src/cpp/galay-utils/cache/type_ring_buffer.hpp"
        "src/cpp/galay-http/client/http_client.h"
        "src/cpp/galay-mcp/v1/server/stdio_server.h")
    if(NOT EXISTS "${GALAY_SOURCE_DIR}/${required_source_path}")
        message(FATAL_ERROR "Missing renamed source path: ${required_source_path}")
    endif()
endforeach()

foreach(forbidden_source_path
        IN ITEMS
        "src/kernel/kernel/runtime.h"
        "src/kernel"
        "src/utils"
        "src/cpp/galay-kernel/concurrency/spsc/ring.h"
        "src/cpp/galay-kernel/concurrency/spsc/unbounded_queue.h"
        "src/cpp/galay-kernel/concurrency/spsc/detail/ring.h"
        "src/cpp/galay-kernel/concurrency/spsc/detail/unbounded_queue.h"
        "src/http"
        "src/mcp"
        "src/galay-kernel"
        "src/galay-utils"
        "src/galay-http"
        "src/galay-mcp")
    if(EXISTS "${GALAY_SOURCE_DIR}/${forbidden_source_path}")
        message(FATAL_ERROR "Unexpected old source path: ${forbidden_source_path}")
    endif()
endforeach()

file(READ "${GALAY_SOURCE_DIR}/CMakeLists.txt" root_cmake_content)
file(READ "${GALAY_SOURCE_DIR}/cmake/option.cmake" option_cmake_content)
if(NOT option_cmake_content MATCHES "option\\(BUILD_TESTING[ \t\r\n]+")
    message(FATAL_ERROR "BUILD_TESTING must be declared in cmake/option.cmake with the other build options.")
endif()
if(root_cmake_content MATCHES "galay-install-prefixed-headers")
    message(FATAL_ERROR "CMake install must use the real src layout, not an install-time header rewrite script.")
endif()
if(NOT root_cmake_content MATCHES "install\\(DIRECTORY[ \t\r\n]+\\$\\{PROJECT_SOURCE_DIR\\}/src/cpp/")
    message(FATAL_ERROR "CMake must directly install C++ headers from PROJECT_SOURCE_DIR/src/cpp/.")
endif()
file(GLOB module_cmake_files
    "${GALAY_SOURCE_DIR}/src/cpp/galay-*/CMakeLists.txt")
foreach(module_cmake_file IN LISTS module_cmake_files)
    file(READ "${module_cmake_file}" module_cmake_content)
    if(module_cmake_content MATCHES "BUILD_INTERFACE:\\$\\{PROJECT_SOURCE_DIR\\}/src/cpp")
        message(FATAL_ERROR "Build targets must include the aggregate build include root, not PROJECT_SOURCE_DIR/src/cpp: ${module_cmake_file}")
    endif()
endforeach()

file(REMOVE_RECURSE "${smoke_root}")
file(MAKE_DIRECTORY "${consumer_source_dir}")

set(install_command
    "${CMAKE_COMMAND}" --install "${GALAY_BINARY_DIR}" --prefix "${prefix_dir}")
if(DEFINED GALAY_INSTALL_CONFIG AND NOT "${GALAY_INSTALL_CONFIG}" STREQUAL "")
    list(APPEND install_command --config "${GALAY_INSTALL_CONFIG}")
endif()

execute_process(
    COMMAND ${install_command}
    RESULT_VARIABLE install_result
    OUTPUT_VARIABLE install_stdout
    ERROR_VARIABLE install_stderr
)

if(NOT install_result EQUAL 0)
    message(FATAL_ERROR
        "Failed to install galay for include layout smoke test.\n"
        "stdout:\n${install_stdout}\n"
        "stderr:\n${install_stderr}")
endif()

foreach(required_header
        IN ITEMS
        "include/galay/cpp/galay-kernel/core/runtime.h"
        "include/galay/cpp/galay-kernel/core/task.h"
        "include/galay/cpp/galay-kernel/common/error.h"
        "include/galay/cpp/galay-kernel/concurrency/spsc/bounded_channel.h"
        "include/galay/cpp/galay-kernel/concurrency/spsc/unbounded_channel.h"
        "include/galay/cpp/galay-utils/cache/ring_buffer.hpp"
        "include/galay/cpp/galay-utils/cache/type_ring_buffer.hpp")
    if(NOT EXISTS "${prefix_dir}/${required_header}")
        message(FATAL_ERROR "Missing installed header: ${required_header}")
    endif()
endforeach()

file(READ "${GALAY_BINARY_DIR}/CMakeCache.txt" galay_cache_content)
if(galay_cache_content MATCHES "GALAY_BUILD_C_API:BOOL=ON")
    foreach(required_c_header
            IN ITEMS
            "include/galay/c/galay-kernel-c/concurrency-c/mpmc/bounded_channel_c.h"
            "include/galay/c/galay-kernel-c/concurrency-c/mpmc/unbounded_channel_c.h"
            "include/galay/c/galay-kernel-c/concurrency-c/mpsc/bounded_channel_c.h"
            "include/galay/c/galay-kernel-c/concurrency-c/mpsc/unbounded_channel_c.h"
            "include/galay/c/galay-kernel-c/concurrency-c/spsc/bounded_channel_c.h"
            "include/galay/c/galay-kernel-c/concurrency-c/spsc/unbounded_channel_c.h")
        if(NOT EXISTS "${prefix_dir}/${required_c_header}")
            message(FATAL_ERROR "Missing installed C API header: ${required_c_header}")
        endif()
    endforeach()
    foreach(legacy_c_header
            IN ITEMS
            "include/galay/c/galay-kernel-c/concurrency-c/channel_c.h"
            "include/galay/c/galay-kernel-c/concurrency-c/bounded_channel_c.h"
            "include/galay/c/galay-kernel-c/concurrency-c/mpsc_channel_c.h"
            "include/galay/c/galay-kernel-c/concurrency-c/unsafe_channel_c.h")
        if(EXISTS "${prefix_dir}/${legacy_c_header}")
            message(FATAL_ERROR "Legacy C API header must not be installed: ${legacy_c_header}")
        endif()
    endforeach()
endif()

foreach(required_module_header
        IN ITEMS
        "include/galay/cpp/galay-ssl/async/ssl_socket.h"
        "include/galay/cpp/galay-http/client/http_client.h"
        "include/galay/cpp/galay-ws/client/ws_client.h"
        "include/galay/cpp/galay-http2/client/h2_client.h"
        "include/galay/cpp/galay-redis/sync/redis_session.h"
        "include/galay/cpp/galay-rpc/kernel/rpc_client.h"
        "include/galay/cpp/galay-mysql/sync/mysql_client.h"
        "include/galay/cpp/galay-mongo/sync/mongo_client.h"
        "include/galay/cpp/galay-etcd/sync/etcd_client.h"
        "include/galay/cpp/galay-mcp/v1/server/stdio_server.h"
        "include/galay/cpp/galay-tracing/kernel/span.h")
    if(NOT EXISTS "${prefix_dir}/${required_module_header}")
        message(FATAL_ERROR "Missing installed module header: ${required_module_header}")
    endif()
endforeach()

foreach(forbidden_header
        IN ITEMS
        "include/kernel/kernel/runtime.h"
        "include/galay-kernel/core/runtime.h"
        "include/galay-kernel/kernel/runtime.h"
        "include/galay/cpp/galay-kernel/concurrency/spsc/ring.h"
        "include/galay/cpp/galay-kernel/concurrency/spsc/unbounded_queue.h"
        "include/galay/cpp/galay-kernel/concurrency/spsc/detail/ring.h"
        "include/galay/cpp/galay-kernel/concurrency/spsc/detail/unbounded_queue.h"
        "include/utils/cache/ring_buffer.hpp"
        "include/http/client/http_client.h"
        "include/mcp/server/stdio_server.h")
    if(EXISTS "${prefix_dir}/${forbidden_header}")
        message(FATAL_ERROR "Unexpected installed header: ${forbidden_header}")
    endif()
endforeach()

file(READ "${prefix_dir}/lib/cmake/galay/galayTargets.cmake" installed_targets_content)
if(installed_targets_content MATCHES "include/galay/cpp")
    message(FATAL_ERROR "Installed CMake targets must expose only the aggregate include root, not include/galay/cpp.")
endif()

foreach(required_package_file
        IN ITEMS
        "lib/cmake/galay/galayConfig.cmake"
        "lib/cmake/galay/galayConfigVersion.cmake"
        "lib/cmake/galay/galayTargets.cmake")
    if(NOT EXISTS "${prefix_dir}/${required_package_file}")
        message(FATAL_ERROR "Missing installed package file: ${required_package_file}")
    endif()
endforeach()

file(GLOB installed_module_package_dirs
    "${prefix_dir}/lib/cmake/galay-*")
if(installed_module_package_dirs)
    message(FATAL_ERROR
        "Installed CMake package layout must expose only lib/cmake/galay, "
        "not per-module package directories: ${installed_module_package_dirs}")
endif()

file(GLOB_RECURSE installed_cppm_files
    "${prefix_dir}/include/galay/cpp/*.cppm")
if(installed_cppm_files)
    message(FATAL_ERROR
        "Installed .cppm files must be provided only by concrete CXX_MODULES targets, "
        "not by the generic header install: ${installed_cppm_files}")
endif()

file(WRITE "${consumer_source_dir}/main.cc"
    "#include <galay/cpp/galay-kernel/core/runtime.h>\n"
    "#include <galay/cpp/galay-kernel/core/awaitable.h>\n"
    "#include <galay/cpp/galay-kernel/async/async_tcp.h>\n"
    "#include <galay/cpp/galay-kernel/common/buffer.h>\n"
    "#include <galay/cpp/galay-kernel/concurrency/spsc/bounded_channel.h>\n"
    "#include <galay/cpp/galay-kernel/concurrency/spsc/unbounded_channel.h>\n"
    "#include <galay/cpp/galay-utils/cache/ring_buffer.hpp>\n"
    "#include <galay/cpp/galay-utils/cache/type_ring_buffer.hpp>\n"
    "int main() {\n"
    "  galay::utils::TypeRingBuffer<int> utils_ring(2);\n"
    "  galay::utils::StaticTypeRingBuffer<int, 2> utils_static_ring;\n"
    "  galay::spsc::Ring<int> kernel_ring(2);\n"
    "  galay::spsc::StaticRing<int, 2> kernel_static_ring;\n"
    "  galay::spsc::UnboundedQueue<int> queue;\n"
    "  return utils_ring.error() == galay::utils::TypeRingBufferError::kNone &&\n"
    "      utils_static_ring.capacity() == 2 &&\n"
    "      kernel_ring.error() == galay::spsc::RingError::kNone &&\n"
    "      kernel_static_ring.capacity() == 2 && queue.valid() ? 0 : 1;\n"
    "}\n")

file(WRITE "${consumer_source_dir}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.20)
project(galay_install_include_layout_consumer LANGUAGES C CXX)

find_package(galay CONFIG REQUIRED)

add_executable(consumer main.cc)
target_compile_features(consumer PRIVATE cxx_std_23)
set_target_properties(consumer PROPERTIES NO_SYSTEM_FROM_IMPORTED ON)
target_link_libraries(consumer PRIVATE galay::kernel)
]=])

if(galay_cache_content MATCHES "GALAY_BUILD_C_API:BOOL=ON")
    file(WRITE "${consumer_source_dir}/c_main.c"
        "#include <galay/c/galay-kernel-c/kernel_c.h>\n"
        "int main(void) {\n"
        "  return galay_kernel_channel_get_error(C_ChannelClosed) == 0;\n"
        "}\n")
    file(APPEND "${consumer_source_dir}/CMakeLists.txt" [=[

add_executable(c_consumer c_main.c)
set_target_properties(c_consumer PROPERTIES NO_SYSTEM_FROM_IMPORTED ON)
target_link_libraries(c_consumer PRIVATE galay::c-kernel)
]=])
endif()

set(configure_command
    "${CMAKE_COMMAND}"
    -S "${consumer_source_dir}"
    -B "${consumer_build_dir}"
    -G "${GALAY_CMAKE_GENERATOR}"
    "-DCMAKE_CXX_COMPILER=${GALAY_CXX_COMPILER}"
    "-DCMAKE_PREFIX_PATH=${prefix_dir}")

execute_process(
    COMMAND ${configure_command}
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_stdout
    ERROR_VARIABLE configure_stderr
)

if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR
        "Failed to configure include layout consumer.\n"
        "stdout:\n${configure_stdout}\n"
        "stderr:\n${configure_stderr}")
endif()

set(build_command "${CMAKE_COMMAND}" --build "${consumer_build_dir}")
if(DEFINED GALAY_INSTALL_CONFIG AND NOT "${GALAY_INSTALL_CONFIG}" STREQUAL "")
    list(APPEND build_command --config "${GALAY_INSTALL_CONFIG}")
endif()

execute_process(
    COMMAND ${build_command}
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_stdout
    ERROR_VARIABLE build_stderr
)

if(NOT build_result EQUAL 0)
    message(FATAL_ERROR
        "Failed to build include layout consumer.\n"
        "stdout:\n${build_stdout}\n"
        "stderr:\n${build_stderr}")
endif()
