cmake_minimum_required(VERSION 3.20)

foreach(required_var
        IN ITEMS
        GALAY_SOURCE_DIR
        GALAY_BINARY_DIR
        GALAY_CMAKE_GENERATOR
        GALAY_CXX_COMPILER)
    if(NOT DEFINED ${required_var} OR "${${required_var}}" STREQUAL "")
        message(FATAL_ERROR "tracing_options_surface requires `${required_var}`.")
    endif()
endforeach()

file(READ "${GALAY_SOURCE_DIR}/cmake/option.cmake" option_cmake_content)
if(option_cmake_content MATCHES "GALAY_TRACING_ENABLE_KERNEL")
    message(FATAL_ERROR "GALAY_TRACING_ENABLE_KERNEL should be removed; kernel integration is already part of the tracing dependency graph.")
endif()
if(NOT option_cmake_content MATCHES "GALAY_TRACING_ENABLE_GALAY_HTTP_OTLP_TRANSPORT")
    message(FATAL_ERROR "Expected explicit option GALAY_TRACING_ENABLE_GALAY_HTTP_OTLP_TRANSPORT.")
endif()

file(READ "${GALAY_SOURCE_DIR}/src/cpp/galay-tracing/CMakeLists.txt" tracing_cmake_content)
if(NOT tracing_cmake_content MATCHES "add_library\\(galay-tracing-kernel[ \t\r\n]+INTERFACE\\)")
    message(FATAL_ERROR "galay-tracing-kernel compatibility target must be created unconditionally.")
endif()
if(tracing_cmake_content MATCHES "if\\(GALAY_TRACING_ENABLE_KERNEL\\)")
    message(FATAL_ERROR "galay-tracing-kernel must not be gated by GALAY_TRACING_ENABLE_KERNEL.")
endif()
if(NOT tracing_cmake_content MATCHES "GALAY_TRACING_ENABLE_GALAY_HTTP_OTLP_TRANSPORT")
    message(FATAL_ERROR "tracing CMake must use the explicit OTLP transport option.")
endif()

set(smoke_root "${GALAY_BINARY_DIR}/test/tracing-options-surface")
set(default_build_dir "${smoke_root}/default-build")
set(transport_build_dir "${smoke_root}/transport-build")
file(REMOVE_RECURSE "${smoke_root}")

set(default_configure_command
    "${CMAKE_COMMAND}"
    -S "${GALAY_SOURCE_DIR}"
    -B "${default_build_dir}"
    -G "${GALAY_CMAKE_GENERATOR}"
    "-DCMAKE_CXX_COMPILER=${GALAY_CXX_COMPILER}"
    -DCMAKE_BUILD_TYPE=Debug
    -DBUILD_TESTING=OFF
    -DGALAY_BUILD_EXAMPLES=OFF
    -DGALAY_BUILD_BENCHMARKS=OFF
    -DGALAY_BUILD_C_API=OFF
    -DGALAY_TRACING_ENABLE_GALAY_HTTP_OTLP_TRANSPORT=OFF)

execute_process(
    COMMAND ${default_configure_command}
    RESULT_VARIABLE default_configure_result
    OUTPUT_VARIABLE default_configure_stdout
    ERROR_VARIABLE default_configure_stderr)
if(NOT default_configure_result EQUAL 0)
    message(FATAL_ERROR
        "Default tracing option configure failed.\n"
        "stdout:\n${default_configure_stdout}\n"
        "stderr:\n${default_configure_stderr}")
endif()

file(READ "${default_build_dir}/compile_commands.json" default_compile_commands)
if(default_compile_commands MATCHES "GALAY_TRACING_ENABLE_OTLP_HTTP")
    message(FATAL_ERROR "Default build must not define the built-in OTLP HTTP transport macro.")
endif()

set(transport_configure_command
    "${CMAKE_COMMAND}"
    -S "${GALAY_SOURCE_DIR}"
    -B "${transport_build_dir}"
    -G "${GALAY_CMAKE_GENERATOR}"
    "-DCMAKE_CXX_COMPILER=${GALAY_CXX_COMPILER}"
    -DCMAKE_BUILD_TYPE=Debug
    -DBUILD_TESTING=OFF
    -DGALAY_BUILD_EXAMPLES=OFF
    -DGALAY_BUILD_BENCHMARKS=OFF
    -DGALAY_BUILD_C_API=OFF
    -DGALAY_TRACING_ENABLE_GALAY_HTTP_OTLP_TRANSPORT=ON)

execute_process(
    COMMAND ${transport_configure_command}
    RESULT_VARIABLE transport_configure_result
    OUTPUT_VARIABLE transport_configure_stdout
    ERROR_VARIABLE transport_configure_stderr)
if(NOT transport_configure_result EQUAL 0)
    message(FATAL_ERROR
        "Transport-enabled tracing option configure failed.\n"
        "stdout:\n${transport_configure_stdout}\n"
        "stderr:\n${transport_configure_stderr}")
endif()

file(READ "${transport_build_dir}/compile_commands.json" transport_compile_commands)
if(NOT transport_compile_commands MATCHES "GALAY_TRACING_ENABLE_OTLP_HTTP")
    message(FATAL_ERROR "Transport-enabled build must keep defining GALAY_TRACING_ENABLE_OTLP_HTTP for source compatibility.")
endif()

set(spdlog_build_dir "${smoke_root}/spdlog-build")
set(spdlog_prefix_dir "${smoke_root}/spdlog-prefix")
set(spdlog_consumer_source_dir "${smoke_root}/spdlog-consumer")
set(spdlog_consumer_build_dir "${smoke_root}/spdlog-consumer-build")

set(spdlog_configure_command
    "${CMAKE_COMMAND}"
    -S "${GALAY_SOURCE_DIR}"
    -B "${spdlog_build_dir}"
    -G "${GALAY_CMAKE_GENERATOR}"
    "-DCMAKE_CXX_COMPILER=${GALAY_CXX_COMPILER}"
    -DCMAKE_BUILD_TYPE=Debug
    -DBUILD_TESTING=OFF
    -DGALAY_BUILD_SSL=OFF
    -DGALAY_BUILD_HTTP=OFF
    -DGALAY_BUILD_WS=OFF
    -DGALAY_BUILD_HTTP2=OFF
    -DGALAY_BUILD_REDIS=OFF
    -DGALAY_BUILD_ETCD=OFF
    -DGALAY_BUILD_MONGO=OFF
    -DGALAY_BUILD_MYSQL=OFF
    -DGALAY_BUILD_RPC=OFF
    -DGALAY_BUILD_MCP=OFF
    -DGALAY_BUILD_EXAMPLES=OFF
    -DGALAY_BUILD_BENCHMARKS=OFF
    -DGALAY_BUILD_C_API=OFF
    -DGALAY_TRACING_ENABLE_SPDLOG=ON)

execute_process(
    COMMAND ${spdlog_configure_command}
    RESULT_VARIABLE spdlog_configure_result
    OUTPUT_VARIABLE spdlog_configure_stdout
    ERROR_VARIABLE spdlog_configure_stderr)

if(spdlog_configure_result EQUAL 0)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" --build "${spdlog_build_dir}" --target galay-tracing galay-tracing-spdlog
        RESULT_VARIABLE spdlog_build_result
        OUTPUT_VARIABLE spdlog_build_stdout
        ERROR_VARIABLE spdlog_build_stderr)
    if(NOT spdlog_build_result EQUAL 0)
        message(FATAL_ERROR
            "Failed to build spdlog tracing targets.\n"
            "stdout:\n${spdlog_build_stdout}\n"
            "stderr:\n${spdlog_build_stderr}")
    endif()

    execute_process(
        COMMAND "${CMAKE_COMMAND}" --install "${spdlog_build_dir}" --prefix "${spdlog_prefix_dir}"
        RESULT_VARIABLE spdlog_install_result
        OUTPUT_VARIABLE spdlog_install_stdout
        ERROR_VARIABLE spdlog_install_stderr)
    if(NOT spdlog_install_result EQUAL 0)
        message(FATAL_ERROR
            "Failed to install spdlog-enabled galay.\n"
            "stdout:\n${spdlog_install_stdout}\n"
            "stderr:\n${spdlog_install_stderr}")
    endif()

    file(READ "${spdlog_prefix_dir}/lib/cmake/galay/galayConfig.cmake" installed_config_content)
    if(NOT installed_config_content MATCHES "find_package\\(spdlog CONFIG QUIET\\)")
        message(FATAL_ERROR "Installed galayConfig.cmake must first try the spdlog CMake package when the adapter target is exported.")
    endif()
    if(NOT installed_config_content MATCHES "find_path\\(GALAY_SPDLOG_INCLUDE_DIR")
        message(FATAL_ERROR "Installed galayConfig.cmake must include the spdlog header/library fallback used by the in-tree build.")
    endif()

    file(MAKE_DIRECTORY "${spdlog_consumer_source_dir}")
    file(WRITE "${spdlog_consumer_source_dir}/main.cc"
        "#include <galay/cpp/galay-tracing/adapters/spdlog_sink.h>\n"
        "#include <spdlog/logger.h>\n"
        "int main() { return 0; }\n")
    file(WRITE "${spdlog_consumer_source_dir}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.20)
project(galay_tracing_spdlog_consumer LANGUAGES CXX)

find_package(galay CONFIG REQUIRED)

add_executable(consumer main.cc)
target_compile_features(consumer PRIVATE cxx_std_23)
target_link_libraries(consumer PRIVATE galay::tracing-spdlog)
]=])

    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            -S "${spdlog_consumer_source_dir}"
            -B "${spdlog_consumer_build_dir}"
            -G "${GALAY_CMAKE_GENERATOR}"
            "-DCMAKE_CXX_COMPILER=${GALAY_CXX_COMPILER}"
            "-DCMAKE_PREFIX_PATH=${spdlog_prefix_dir}"
        RESULT_VARIABLE spdlog_consumer_configure_result
        OUTPUT_VARIABLE spdlog_consumer_configure_stdout
        ERROR_VARIABLE spdlog_consumer_configure_stderr)
    if(NOT spdlog_consumer_configure_result EQUAL 0)
        message(FATAL_ERROR
            "Failed to configure spdlog adapter consumer.\n"
            "stdout:\n${spdlog_consumer_configure_stdout}\n"
            "stderr:\n${spdlog_consumer_configure_stderr}")
    endif()
else()
    if(spdlog_configure_stderr MATCHES "spdlog not found" OR
       spdlog_configure_stdout MATCHES "spdlog not found")
        message(STATUS "Skipping spdlog install dependency smoke because spdlog is unavailable in this environment.")
    else()
        message(FATAL_ERROR
            "Failed to configure spdlog-enabled tracing option smoke.\n"
            "stdout:\n${spdlog_configure_stdout}\n"
            "stderr:\n${spdlog_configure_stderr}")
    endif()
endif()
