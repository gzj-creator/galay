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
        "src/c/galay-kernel-c/kernel.h"
        "src/c/galay-common-c/common/macro.h"
        "src/c/galay-utils-c/macro.h"
        "src/c/galay-kernel-c/common-c/macro.h"
        "src/c/galay-http2-c/macro.h"
        "src/c/galay-rpc-c/macro.h"
        "src/c/galay-mongo-c/macro.h"
        "src/c/galay-tracing-c/macro.h"
        "src/c/galay-kernel-c/concurrency-c/mpmc/bounded_channel.h"
        "src/c/galay-kernel-c/concurrency-c/mpsc/bounded_channel.h"
        "src/c/galay-kernel-c/concurrency-c/spsc/bounded_channel.h"
        "src/cpp/galay-utils/cache/ring_buffer.hpp"
        "src/cpp/galay-utils/common/macro.hpp"
        "src/cpp/galay-utils/cache/type_ring_buffer.hpp"
        "thirdparty/concurrentqueue/moodycamel/concurrentqueue.h"
        "thirdparty/concurrentqueue/moodycamel/blockingconcurrentqueue.h"
        "thirdparty/concurrentqueue/moodycamel/lightweightsemaphore.h"
        "thirdparty/concurrentqueue/LICENSE.md"
        "thirdparty/concurrentqueue/README.md"
        "src/cpp/galay-http/common/macro.hpp"
        "src/cpp/galay-http/client/http_client.h"
        "src/cpp/galay-ws/common/macro.hpp"
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

# The workspace may contain an install manifest created by a different user.
# Use a private staging build tree so CMake can safely write its manifest.
set(install_stage_dir "${smoke_root}/install-stage")
file(MAKE_DIRECTORY "${install_stage_dir}")
file(REMOVE "${GALAY_BINARY_DIR}/install_manifest.txt")

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E chdir "${install_stage_dir}" ${install_command}
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
        "include/galay/cpp/galay-utils/cache/type_ring_buffer.hpp"
        "include/galay/thirdparty/concurrentqueue/moodycamel/concurrentqueue.h"
        "include/galay/thirdparty/concurrentqueue/moodycamel/blockingconcurrentqueue.h"
        "include/galay/thirdparty/concurrentqueue/moodycamel/lightweightsemaphore.h"
        "include/galay/thirdparty/concurrentqueue/LICENSE.md"
        "include/galay/thirdparty/concurrentqueue/README.md")
    if(NOT EXISTS "${prefix_dir}/${required_header}")
        message(FATAL_ERROR "Missing installed header: ${required_header}")
    endif()
endforeach()

file(READ "${GALAY_BINARY_DIR}/CMakeCache.txt" galay_cache_content)
if(galay_cache_content MATCHES "GALAY_BUILD_C_API:BOOL=ON")
    foreach(required_c_header
            IN ITEMS
            "include/galay/c/galay-kernel-c/kernel.h"
            "include/galay/c/galay-kernel-c/concurrency-c/mpmc/bounded_channel.h"
            "include/galay/c/galay-kernel-c/concurrency-c/mpsc/bounded_channel.h"
            "include/galay/c/galay-kernel-c/concurrency-c/spsc/bounded_channel.h")
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
if(installed_targets_content MATCHES "INTERFACE_INCLUDE_DIRECTORIES[^\n]*include/galay/cpp")
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
file(GLOB installed_module_dirs
    "${prefix_dir}/include/galay/cpp/galay-*/module")
if(galay_cache_content MATCHES "GALAY_INSTALL_CPP23_MODULE_INTERFACES:BOOL=OFF")
    if(installed_module_dirs)
        message(FATAL_ERROR
            "C++ module directories must not be installed when "
            "GALAY_INSTALL_CPP23_MODULE_INTERFACES=OFF: ${installed_module_dirs}")
    endif()
    if(installed_cppm_files)
        message(FATAL_ERROR
            "Installed .cppm files must not be present when "
            "GALAY_INSTALL_CPP23_MODULE_INTERFACES=OFF: ${installed_cppm_files}")
    endif()
elseif(installed_cppm_files)
    foreach(installed_cppm_file IN LISTS installed_cppm_files)
        if(NOT installed_cppm_file MATCHES "/module/[^/]+\\.cppm$")
            message(FATAL_ERROR
                "Installed .cppm files must live in a module directory: ${installed_cppm_file}")
        endif()
    endforeach()
endif()

if(galay_cache_content MATCHES "GALAY_INSTALL_CPP23_MODULE_INTERFACES:BOOL=ON")
    set(expected_module_interfaces
        "GALAY_BUILD_UTILS:galay-utils/module/galay_utils.cppm"
        "GALAY_BUILD_KERNEL:galay-kernel/module/galay_kernel.cppm"
        "GALAY_BUILD_SSL:galay-ssl/module/galay_ssl.cppm"
        "GALAY_BUILD_HTTP:galay-http/module/galay_http.cppm"
        "GALAY_BUILD_WS:galay-ws/module/galay_websocket.cppm"
        "GALAY_BUILD_HTTP2:galay-http2/module/galay_http2.cppm"
        "GALAY_BUILD_REDIS:galay-redis/module/galay_redis.cppm"
        "GALAY_BUILD_ETCD:galay-etcd/module/galay_etcd.cppm"
        "GALAY_BUILD_MYSQL:galay-mysql/module/galay_mysql.cppm"
        "GALAY_BUILD_POSTGRES:galay-postgres/module/galay_postgres.cppm"
        "GALAY_BUILD_MONGO:galay-mongo/module/galay_mongo.cppm"
        "GALAY_BUILD_RPC:galay-rpc/module/galay_rpc.cppm"
        "GALAY_BUILD_MCP:galay-mcp/module/galay_mcp.cppm"
        "GALAY_BUILD_TRACING:galay-tracing/module/galay_tracing.cppm")
    if(galay_cache_content MATCHES "GALAY_RPC_ENABLE_ETCD:BOOL=ON")
        list(APPEND expected_module_interfaces
            "GALAY_BUILD_RPC:galay-rpc/module/galay_rpc_etcd.cppm")
    endif()

    foreach(expected_module_interface IN LISTS expected_module_interfaces)
        string(REPLACE ":" ";" expected_module_parts "${expected_module_interface}")
        list(GET expected_module_parts 0 module_build_option)
        list(GET expected_module_parts 1 module_interface)
        if(galay_cache_content MATCHES "${module_build_option}:BOOL=ON")
            set(installed_module_interface "${prefix_dir}/include/galay/cpp/${module_interface}")
            if(NOT EXISTS "${installed_module_interface}")
                message(FATAL_ERROR
                    "Missing installed C++ module interface: ${installed_module_interface}")
            endif()
            get_filename_component(module_directory "${module_interface}" DIRECTORY)
            set(installed_module_prelude
                "${prefix_dir}/include/galay/cpp/${module_directory}/module_prelude.hpp")
            if(NOT EXISTS "${installed_module_prelude}")
                message(FATAL_ERROR
                    "Missing installed C++ module prelude: ${installed_module_prelude}")
            endif()
        endif()
    endforeach()
endif()

file(WRITE "${consumer_source_dir}/main.cc"
    "#include <galay/cpp/galay-kernel/core/runtime.h>\n"
    "#include <galay/cpp/galay-kernel/core/awaitable.h>\n"
    "#include <galay/cpp/galay-kernel/async/async_tcp.h>\n"
    "#include <galay/cpp/galay-kernel/common/buffer.h>\n"
    "#include <galay/cpp/galay-kernel/concurrency/spsc/bounded_channel.h>\n"
    "#include <galay/cpp/galay-kernel/concurrency/spsc/unbounded_channel.h>\n"
    "#include <galay/cpp/galay-utils/common/macro.hpp>\n"
    "#include <galay/cpp/galay-utils/cache/ring_buffer.hpp>\n"
    "#include <galay/cpp/galay-utils/cache/type_ring_buffer.hpp>\n"
    "#include <galay/thirdparty/concurrentqueue/moodycamel/concurrentqueue.h>\n"
    "#include <galay/cpp/galay-http/common/macro.hpp>\n"
    "#include <galay/cpp/galay-ws/common/macro.hpp>\n"
    "int main() {\n"
    "  galay::utils::TypeRingBuffer<int> utils_ring(2);\n"
    "  galay::utils::StaticTypeRingBuffer<int, 2> utils_static_ring;\n"
    "  galay::spsc::Ring<int> kernel_ring(2);\n"
    "  galay::spsc::StaticRing<int, 2> kernel_static_ring;\n"
    "  galay::spsc::UnboundedQueue<int> queue;\n"
    "  moodycamel::ConcurrentQueue<int> vendor_queue;\n"
    "  int vendor_value = 0;\n"
    "  vendor_queue.enqueue(42);\n"
    "  return DEFAULT_HTTP_MAX_HEADER_SIZE == 8192 &&\n"
    "      utils_ring.error() == galay::utils::TypeRingBufferError::kNone &&\n"
    "      utils_static_ring.capacity() == 2 &&\n"
    "      kernel_ring.error() == galay::spsc::RingError::kNone &&\n"
    "      kernel_static_ring.capacity() == 2 && queue.valid() &&\n"
    "      vendor_queue.try_dequeue(vendor_value) && vendor_value == 42 ? 0 : 1;\n"
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
        "#include <galay/c/galay-common-c/common/macro.h>\n"
        "#include <galay/c/galay-http2-c/macro.h>\n"
        "#include <galay/c/galay-mongo-c/macro.h>\n"
        "#include <galay/c/galay-rpc-c/macro.h>\n"
        "#include <galay/c/galay-tracing-c/macro.h>\n"
        "#include <galay/c/galay-utils-c/macro.h>\n"
        "#include <galay/c/galay-kernel-c/kernel.h>\n"
        "int main(void) {\n"
        "  C_RuntimeConfig config = galay_c_runtime_config_default();\n"
        "  return config.io_scheduler_count == C_RUNTIME_SCHEDULER_COUNT_AUTO &&\n"
        "      GALAY_HTTP2_FRAME_HEADER_LENGTH == 9u &&\n"
        "      GALAY_MONGO_MAX_KEY_LENGTH == 255u &&\n"
        "      GALAY_RPC_HEADER_SIZE == 16u &&\n"
        "      GALAY_TRACING_TRACE_ID_HEX_LENGTH == 32u &&\n"
        "      GALAY_UTILS_OK == GALAY_OK ? 0 : 1;\n"
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
