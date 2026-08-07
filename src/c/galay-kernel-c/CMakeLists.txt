enable_language(ASM)

set(GALAY_C_CORO_CONTEXT_SUPPORTED OFF CACHE INTERNAL "C coroutine context switch support")
set(GALAY_C_CORO_CONTEXT_UNSUPPORTED_REASON "" CACHE INTERNAL "C coroutine context unsupported reason")

file(GLOB galay_c_kernel_sources CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/core-c/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/async-c/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/concurrency-c/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/concurrency-c/*/*.cc"
    "${CMAKE_CURRENT_SOURCE_DIR}/coro-c/*.cc"
)

if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    set(GALAY_C_CORO_CONTEXT_SUPPORTED ON CACHE INTERNAL "C coroutine context switch support" FORCE)
    list(APPEND galay_c_kernel_sources
        "${CMAKE_CURRENT_SOURCE_DIR}/coro-c/coro_context_aarch64.S"
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    set(GALAY_C_CORO_CONTEXT_SUPPORTED ON CACHE INTERNAL "C coroutine context switch support" FORCE)
    list(APPEND galay_c_kernel_sources
        "${CMAKE_CURRENT_SOURCE_DIR}/coro-c/coro_context_x86_64.S"
    )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
    set(GALAY_C_CORO_CONTEXT_UNSUPPORTED_REASON
        "Linux/aarch64 stackful C coroutine context switch is not implemented; C coroutine APIs return ENOTSUP and coroutine-dependent C tests/benchmarks/examples are skipped."
        CACHE INTERNAL "C coroutine context unsupported reason" FORCE)
else()
    set(GALAY_C_CORO_CONTEXT_UNSUPPORTED_REASON
        "${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR} stackful C coroutine context switch is not implemented; C coroutine APIs return ENOTSUP and coroutine-dependent C tests/benchmarks/examples are skipped."
        CACHE INTERNAL "C coroutine context unsupported reason" FORCE)
endif()

if(GALAY_C_CORO_CONTEXT_SUPPORTED)
    message(STATUS "C stackful coroutine context support: enabled for ${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}")
else()
    message(STATUS "C stackful coroutine context support: disabled. ${GALAY_C_CORO_CONTEXT_UNSUPPORTED_REASON}")
endif()

function(galay_filter_unsupported_c_coro_sources source_list)
    if(GALAY_C_CORO_CONTEXT_SUPPORTED)
        return()
    endif()

    set(filtered_sources)
    foreach(source_file IN LISTS ${source_list})
        file(READ "${source_file}" source_text)
        string(FIND "${source_text}" "galay_coro_spawn" spawn_position)
        if(spawn_position EQUAL -1)
            list(APPEND filtered_sources "${source_file}")
        else()
            get_filename_component(source_name "${source_file}" NAME_WE)
            message(STATUS "Skipping ${source_name}: ${GALAY_C_CORO_CONTEXT_UNSUPPORTED_REASON}")
        endif()
    endforeach()
    set(${source_list} "${filtered_sources}" PARENT_SCOPE)
endfunction()

add_library(galay-c-kernel ${galay_c_kernel_sources})
add_library(galay::c-kernel ALIAS galay-c-kernel)

target_compile_features(galay-c-kernel PUBLIC cxx_std_23)

target_include_directories(galay-c-kernel
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(galay-c-kernel
    PUBLIC
        galay::c-bridge
        galay::kernel
)

set_target_properties(galay-c-kernel PROPERTIES EXPORT_NAME c-kernel)
