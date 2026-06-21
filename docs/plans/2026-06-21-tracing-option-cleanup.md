# Tracing Option Cleanup Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Simplify `galay-tracing` build options by removing the redundant kernel adapter switch, preserving external dependency isolation for spdlog, and clarifying that OTLP HTTP gating controls the built-in `galay-http` transport rather than the exporter itself.

**Architecture:** Keep `galay::tracing` as the core tracing target and keep optional integrations at CMake target boundaries. The kernel compatibility target is always exported because the core target already depends on `galay-kernel`; spdlog remains an opt-in adapter; the built-in OTLP transport gets a clearer option name while retaining the old option as a compatibility alias for one transition.

**Tech Stack:** CMake 3.20, C++23, CTest, Galay module targets, installed CMake package smoke tests.

---

### Task 1: Add CMake Surface Regression Checks

**Files:**
- Modify: `test/cpp/config/CMakeLists.txt`
- Create: `test/cpp/config/tracing_options_surface.cmake`

**Step 1: Write the failing test**

Create `test/cpp/config/tracing_options_surface.cmake` with checks that prove the desired surface:

```cmake
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
```

Register it in `test/cpp/config/CMakeLists.txt`:

```cmake
add_test(
    NAME config.tracing_options_surface
    COMMAND ${CMAKE_COMMAND}
        -D GALAY_SOURCE_DIR=${PROJECT_SOURCE_DIR}
        -D GALAY_BINARY_DIR=${CMAKE_BINARY_DIR}
        -D GALAY_CMAKE_GENERATOR=${CMAKE_GENERATOR}
        -D GALAY_CXX_COMPILER=${CMAKE_CXX_COMPILER}
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tracing_options_surface.cmake
)
```

**Step 2: Run test to verify it fails**

Run:

```bash
rtk cmake -S . -B build-tracing-option-cleanup -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DGALAY_BUILD_EXAMPLES=OFF -DGALAY_BUILD_BENCHMARKS=OFF -DGALAY_BUILD_C_API=OFF
rtk cmake --build build-tracing-option-cleanup --target t_module_config_surface
rtk ctest --test-dir build-tracing-option-cleanup -R '^config.tracing_options_surface$' --output-on-failure
```

Expected: `config.tracing_options_surface` fails because `GALAY_TRACING_ENABLE_KERNEL` still exists and the new transport option does not.

**Step 3: Do not change production code in this task**

This task only adds the RED test and CTest registration.

### Task 2: Remove Redundant Kernel Option And Rename OTLP Transport Switch

**Files:**
- Modify: `cmake/option.cmake`
- Modify: `src/cpp/galay-tracing/CMakeLists.txt`
- Modify: `test/cpp/tracing/CMakeLists.txt`
- Modify: `docs/modules/tracing/04-导出到Collector.md`
- Modify: `docs/modules/tracing/plans/2026-05-19-galay-tracing-implementation.md`
- Modify: `docs/modules/tracing/plans/2026-06-19-tracing-production-hardening.md`
- Modify: `docs/plans/2026-06-13-galay-monorepo-modules.md`

**Step 1: Run RED from Task 1**

Run:

```bash
rtk ctest --test-dir build-tracing-option-cleanup -R '^config.tracing_options_surface$' --output-on-failure
```

Expected: FAIL for the old option surface.

**Step 2: Update options**

In `cmake/option.cmake`:

```cmake
option(GALAY_TRACING_ENABLE_SPDLOG "Enable the tracing spdlog adapter" OFF)
option(GALAY_TRACING_ENABLE_GALAY_HTTP_OTLP_TRANSPORT "Enable the built-in galay-http OTLP transport" OFF)

if(DEFINED GALAY_TRACING_ENABLE_OTLP_HTTP)
    message(DEPRECATION
        "GALAY_TRACING_ENABLE_OTLP_HTTP is deprecated; use "
        "GALAY_TRACING_ENABLE_GALAY_HTTP_OTLP_TRANSPORT.")
    set(GALAY_TRACING_ENABLE_GALAY_HTTP_OTLP_TRANSPORT
        "${GALAY_TRACING_ENABLE_OTLP_HTTP}"
        CACHE BOOL "Enable the built-in galay-http OTLP transport" FORCE)
endif()
```

Remove `GALAY_TRACING_ENABLE_KERNEL`.

**Step 3: Update tracing CMake**

In `src/cpp/galay-tracing/CMakeLists.txt`:

- Replace all CMake conditionals that currently use `GALAY_TRACING_ENABLE_OTLP_HTTP` with `GALAY_TRACING_ENABLE_GALAY_HTTP_OTLP_TRANSPORT`.
- Keep the public compile definition as `GALAY_TRACING_ENABLE_OTLP_HTTP=1` when the new option is ON. This preserves source compatibility for headers/tests using `#if defined(GALAY_TRACING_ENABLE_OTLP_HTTP)`.
- Create `galay-tracing-kernel` unconditionally:

```cmake
add_library(galay-tracing-kernel INTERFACE)
add_library(galay-tracing::galay-tracing-kernel ALIAS galay-tracing-kernel)
target_compile_features(galay-tracing-kernel INTERFACE cxx_std_23)
target_include_directories(galay-tracing-kernel
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(galay-tracing-kernel INTERFACE tracing ${_galay_tracing_kernel_target})
set_target_properties(galay-tracing-kernel PROPERTIES EXPORT_NAME tracing-kernel)
```

**Step 4: Update tracing tests**

In `test/cpp/tracing/CMakeLists.txt`, remove the `if(GALAY_TRACING_ENABLE_KERNEL)` gate around `T8-kernel_context` so the test always builds and runs when `GALAY_BUILD_TRACING` is ON.

**Step 5: Update docs and command examples**

Replace user-facing references:

- `GALAY_TRACING_ENABLE_KERNEL` should be removed from active docs or marked obsolete if historical context must remain.
- `GALAY_TRACING_ENABLE_OTLP_HTTP` should become `GALAY_TRACING_ENABLE_GALAY_HTTP_OTLP_TRANSPORT` in active commands and docs.
- Keep source-level text clear: the exporter exists by default; only the built-in `galay-http` transport is gated.

**Step 6: Run targeted tests**

Run:

```bash
rtk cmake -S . -B build-tracing-option-cleanup -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DGALAY_BUILD_EXAMPLES=OFF -DGALAY_BUILD_BENCHMARKS=OFF -DGALAY_BUILD_C_API=OFF
rtk cmake --build build-tracing-option-cleanup --target T8-kernel_context T9-otlp_http_exporter t_module_config_surface
rtk ctest --test-dir build-tracing-option-cleanup -R '^(config.tracing_options_surface|tracing.kernel_context|tracing.otlp_http_exporter)$' --output-on-failure
```

Expected: PASS.

**Step 7: Verify compatibility alias**

Run:

```bash
rtk cmake -S . -B build-tracing-option-cleanup-compat -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DGALAY_BUILD_EXAMPLES=OFF -DGALAY_BUILD_BENCHMARKS=OFF -DGALAY_BUILD_C_API=OFF -DGALAY_TRACING_ENABLE_OTLP_HTTP=ON
rtk cmake --build build-tracing-option-cleanup-compat --target T9-otlp_http_exporter
```

Expected: configure succeeds with a deprecation warning and build succeeds.

### Task 3: Fix Installed Package Dependencies For Optional Tracing Targets

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `cmake/galayConfig.cmake.in`
- Modify: `test/cpp/config/tracing_options_surface.cmake`

**Step 1: Extend the failing test**

Add an installed-package smoke case to `test/cpp/config/tracing_options_surface.cmake`:

```cmake
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
        COMMAND "${CMAKE_COMMAND}" --build "${spdlog_build_dir}" --target tracing galay-tracing-spdlog
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
    if(NOT installed_config_content MATCHES "find_dependency\\(spdlog CONFIG\\)")
        message(FATAL_ERROR "Installed galayConfig.cmake must find spdlog when the spdlog adapter target is exported.")
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
    message(STATUS "Skipping spdlog install dependency smoke because spdlog is unavailable in this environment.")
endif()
```

**Step 2: Run test to verify it fails when spdlog is available**

Run:

```bash
rtk ctest --test-dir build-tracing-option-cleanup -R '^config.tracing_options_surface$' --output-on-failure
```

Expected when spdlog is available: FAIL because installed `galayConfig.cmake` does not call `find_dependency(spdlog CONFIG)`.

Expected when spdlog is unavailable: SKIP via `message(STATUS ...)`; continue with production changes but report the local limitation.

**Step 3: Add package dependency flags**

In `CMakeLists.txt`, before `configure_package_config_file(...)`, add:

```cmake
set(GALAY_CONFIG_NEEDS_SPDLOG OFF)
if(TARGET galay-tracing-spdlog)
    set(GALAY_CONFIG_NEEDS_SPDLOG ON)
endif()
```

In `cmake/galayConfig.cmake.in`, add:

```cmake
if(@GALAY_CONFIG_NEEDS_SPDLOG@)
    find_dependency(spdlog CONFIG)
endif()
```

Keep this conditional. Do not require spdlog for default installs.

**Step 4: Run targeted package tests**

Run:

```bash
rtk cmake -S . -B build-tracing-option-cleanup -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DGALAY_BUILD_EXAMPLES=OFF -DGALAY_BUILD_BENCHMARKS=OFF -DGALAY_BUILD_C_API=OFF
rtk cmake --build build-tracing-option-cleanup --target t_module_config_surface
rtk ctest --test-dir build-tracing-option-cleanup -R '^(config.tracing_options_surface|config.install_include_layout)$' --output-on-failure
```

Expected: PASS, or spdlog subcase explicitly skipped only when the environment lacks spdlog.

### Task 4: Final Verification

**Files:**
- No code changes expected.

**Step 1: Format and diff checks**

Run:

```bash
rtk git diff --check
rtk git status --short
```

Expected: `git diff --check` exits 0. `git status --short` contains only intentional files.

**Step 2: Default tracing verification**

Run:

```bash
rtk cmake -S . -B build-tracing-option-cleanup -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DGALAY_BUILD_EXAMPLES=OFF -DGALAY_BUILD_BENCHMARKS=OFF -DGALAY_BUILD_C_API=OFF
rtk cmake --build build-tracing-option-cleanup --parallel
rtk ctest --test-dir build-tracing-option-cleanup -R '^(config\\.|tracing\\.)' --output-on-failure
```

Expected: all config and tracing tests pass.

**Step 3: Transport-enabled verification**

Run:

```bash
rtk cmake -S . -B build-tracing-option-cleanup-transport -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DGALAY_BUILD_EXAMPLES=OFF -DGALAY_BUILD_BENCHMARKS=OFF -DGALAY_BUILD_C_API=OFF -DGALAY_TRACING_ENABLE_GALAY_HTTP_OTLP_TRANSPORT=ON
rtk cmake --build build-tracing-option-cleanup-transport --target T9-otlp_http_exporter
rtk ctest --test-dir build-tracing-option-cleanup-transport -R '^tracing.otlp_http_exporter$' --output-on-failure
```

Expected: `tracing.otlp_http_exporter` passes with the built-in transport code compiled.

**Step 4: Benchmark build smoke**

Run:

```bash
rtk cmake -S . -B build-tracing-option-cleanup-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DGALAY_BUILD_EXAMPLES=OFF -DGALAY_BUILD_BENCHMARKS=ON -DGALAY_BUILD_C_API=OFF
rtk cmake --build build-tracing-option-cleanup-release --target benchmark_tracing_otlp_json_exporter_throughput benchmark_tracing_batch_processor_schedule_throughput
```

Expected: benchmark targets build. Benchmark runtime output is not a pass/fail gate for this cleanup.

**Step 5: Final review**

Review for:

- No `GALAY_TRACING_ENABLE_KERNEL` in active source/CMake/docs except changelog-like historical text if intentionally preserved.
- `GALAY_TRACING_ENABLE_OTLP_HTTP` remains only as source compatibility macro and deprecated CMake alias.
- Default build does not require spdlog or the built-in HTTP transport.
- Installed package finds spdlog only when the adapter target was exported.
