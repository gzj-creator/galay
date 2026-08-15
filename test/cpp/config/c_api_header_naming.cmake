cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED GALAY_SOURCE_DIR OR "${GALAY_SOURCE_DIR}" STREQUAL "")
    message(FATAL_ERROR "c_api_header_naming requires `GALAY_SOURCE_DIR`.")
endif()

set(public_c_modules
    etcd
    http
    http2
    mcp
    mongo
    mysql
    redis
    rpc
    ssl
    tracing
    utils
    ws
)

foreach(module IN LISTS public_c_modules)
    set(module_dir "${GALAY_SOURCE_DIR}/src/c/galay-${module}-c")
    if(NOT IS_DIRECTORY "${module_dir}")
        message(FATAL_ERROR "Missing C ABI module directory: ${module_dir}")
    endif()
    if(NOT EXISTS "${module_dir}/${module}.h")
        message(FATAL_ERROR "Missing canonical C ABI public header: ${module_dir}/${module}.h")
    endif()
    file(READ "${module_dir}/${module}.h" module_header_content)
    if(module_header_content MATCHES "#ifndef[ \t]+[A-Z0-9_]+_C_H[\r\n]")
        message(FATAL_ERROR
            "C ABI include guard must match the suffix-free header name: ${module_dir}/${module}.h")
    endif()
    if(NOT EXISTS "${module_dir}/${module}.c" AND
       NOT EXISTS "${module_dir}/${module}.cc")
        message(FATAL_ERROR
            "Missing canonical C ABI implementation source: ${module_dir}/${module}.c or .cc")
    endif()
endforeach()

if(NOT EXISTS "${GALAY_SOURCE_DIR}/src/c/galay-kernel-c/kernel.h")
    message(FATAL_ERROR "Missing kernel C ABI umbrella header: src/c/galay-kernel-c/kernel.h")
endif()

foreach(topology IN ITEMS mpmc mpsc spsc)
    set(channel_header
        "${GALAY_SOURCE_DIR}/src/c/galay-kernel-c/concurrency-c/${topology}/bounded_channel.h")
    if(NOT EXISTS "${channel_header}")
        message(FATAL_ERROR
            "Missing canonical ${topology} bounded C ABI header: ${channel_header}")
    endif()
endforeach()

file(GLOB_RECURSE legacy_c_suffix_files
    "${GALAY_SOURCE_DIR}/src/c/*_c.c"
    "${GALAY_SOURCE_DIR}/src/c/*_c.cc"
    "${GALAY_SOURCE_DIR}/src/c/*_c.h")
if(legacy_c_suffix_files)
    message(FATAL_ERROR
        "C ABI source/header filenames must not retain the _c suffix: ${legacy_c_suffix_files}")
endif()

file(GLOB_RECURSE kernel_c_sources
    LIST_DIRECTORIES false
    "${GALAY_SOURCE_DIR}/src/c/galay-kernel-c/*.c"
    "${GALAY_SOURCE_DIR}/src/c/galay-kernel-c/*.h")
foreach(kernel_c_source IN LISTS kernel_c_sources)
    file(READ "${kernel_c_source}" kernel_c_content)
    if(kernel_c_content MATCHES "(^|[^A-Za-z0-9_])galay_kernel_")
        message(FATAL_ERROR
            "Kernel C ABI identifiers must use the galay_c_ prefix: ${kernel_c_source}")
    endif()
    if(kernel_c_content MATCHES "(^|[^A-Za-z0-9_])galay_coro_")
        message(FATAL_ERROR
            "Kernel coroutine C ABI identifiers must use the galay_c_coro_ prefix: ${kernel_c_source}")
    endif()
    if(kernel_c_content MATCHES "#ifndef[ \t]+GALAY_KERNEL_")
        message(FATAL_ERROR
            "Kernel C ABI include guards must use the GALAY_C_KERNEL_ prefix: ${kernel_c_source}")
    endif()
    if(kernel_c_content MATCHES "@file[ \t]+[A-Za-z0-9_]+_(c|native)\\.h")
        message(FATAL_ERROR
            "C ABI @file name must match the suffix-free header: ${kernel_c_source}")
    endif()
endforeach()

file(READ "${GALAY_SOURCE_DIR}/cmake/option.cmake" option_cmake_content)
if(NOT option_cmake_content MATCHES
        "option\\(GALAY_BUILD_C_API[ \t]+\"Build C ABI wrapper targets\"[ \t]+ON\\)")
    message(FATAL_ERROR "GALAY_BUILD_C_API must be enabled by default.")
endif()

if(EXISTS "${GALAY_SOURCE_DIR}/.wroktree")
    message(FATAL_ERROR "Typo local worktree directory remains: .wroktree")
endif()

file(READ "${GALAY_SOURCE_DIR}/.gitignore" gitignore_content)
if(gitignore_content MATCHES "(^|\n)\\.wroktree/")
    message(FATAL_ERROR ".gitignore must not keep the misspelled .wroktree entry.")
endif()
if(NOT gitignore_content MATCHES "(^|\n)\\.worktree/")
    message(FATAL_ERROR ".gitignore must ignore the canonical .worktree directory.")
endif()
