cmake_minimum_required(VERSION 3.20)

if(NOT DEFINED GALAY_SOURCE_DIR OR "${GALAY_SOURCE_DIR}" STREQUAL "")
    message(FATAL_ERROR "kernel_internal_includes_relative requires `GALAY_SOURCE_DIR`.")
endif()

file(GLOB_RECURSE kernel_sources
    "${GALAY_SOURCE_DIR}/src/cpp/galay-kernel/*.cc"
    "${GALAY_SOURCE_DIR}/src/cpp/galay-kernel/*.h"
    "${GALAY_SOURCE_DIR}/src/cpp/galay-kernel/*.hpp"
    "${GALAY_SOURCE_DIR}/src/cpp/galay-kernel/*.inl")

set(public_self_includes)
foreach(kernel_source IN LISTS kernel_sources)
    file(READ "${kernel_source}" source_content)
    if(source_content MATCHES "#include[ \t]*[<\"]galay/cpp/galay-kernel/")
        file(RELATIVE_PATH relative_source "${GALAY_SOURCE_DIR}" "${kernel_source}")
        list(APPEND public_self_includes "${relative_source}")
    endif()
endforeach()

if(public_self_includes)
    list(JOIN public_self_includes "\n  " formatted_public_self_includes)
    message(FATAL_ERROR
        "galay-kernel internal sources must include kernel headers by relative path, "
        "not through the installed public include prefix:\n  ${formatted_public_self_includes}")
endif()
