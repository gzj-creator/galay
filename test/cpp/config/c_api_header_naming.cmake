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
    if(EXISTS "${module_dir}/${module}.h")
        message(FATAL_ERROR "C ABI public header must be renamed to ${module}_c.h: ${module_dir}/${module}.h")
    endif()
    if(NOT EXISTS "${module_dir}/${module}_c.h")
        message(FATAL_ERROR "Missing canonical C ABI public header: ${module_dir}/${module}_c.h")
    endif()
endforeach()

if(NOT EXISTS "${GALAY_SOURCE_DIR}/src/c/galay-kernel-c/kernel_c.h")
    message(FATAL_ERROR "Missing kernel C ABI umbrella header: src/c/galay-kernel-c/kernel_c.h")
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
