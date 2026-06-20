if(TARGET galay::kernel)
    set(_galay_tracing_kernel_target galay::kernel)
elseif(TARGET galay-kernel)
    add_library(galay::kernel ALIAS galay-kernel)
    set(_galay_tracing_kernel_target galay::kernel)
elseif(TARGET kernel)
    add_library(galay::kernel ALIAS kernel)
    set(_galay_tracing_kernel_target galay::kernel)
elseif(TARGET galay-kernel::galay-kernel)
    set(_galay_tracing_kernel_target galay-kernel::galay-kernel)
else()
    message(FATAL_ERROR "tracing requires an in-tree kernel target")
endif()

set(GALAY_TRACING_SOURCES
    common/id_format.cc
    context/context_storage.cc
    context/traceparent.cc
    kernel/batch_span_processor.cc
    kernel/file_span_exporter.cc
    kernel/otlp_http_exporter.cc
    kernel/sampler.cc
    kernel/span.cc
    kernel/span_guard.cc
    log/console_sink.cc
    log/logger.cc
)

if(GALAY_TRACING_ENABLE_OTLP_HTTP)
    if(TARGET galay::http)
        set(_galay_tracing_http_target galay::http)
    elseif(TARGET http)
        add_library(galay::http ALIAS http)
        set(_galay_tracing_http_target galay::http)
    elseif(TARGET galay-http)
        add_library(galay::http ALIAS galay-http)
        set(_galay_tracing_http_target galay::http)
    elseif(TARGET galay-http::galay-http)
        set(_galay_tracing_http_target galay-http::galay-http)
    else()
        message(FATAL_ERROR "tracing OTLP/HTTP requires an in-tree http target")
    endif()

endif()

add_library(tracing ${GALAY_TRACING_SOURCES})
add_library(galay::tracing ALIAS tracing)

if(NOT TARGET galay-tracing)
    add_library(galay-tracing ALIAS tracing)
endif()
if(NOT TARGET galay-tracing::galay-tracing)
    add_library(galay-tracing::galay-tracing ALIAS tracing)
endif()

target_compile_features(tracing PUBLIC cxx_std_23)
target_include_directories(tracing
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_link_libraries(tracing PUBLIC ${_galay_tracing_kernel_target})

if(GALAY_TRACING_ENABLE_OTLP_HTTP)
    target_compile_definitions(tracing PUBLIC GALAY_TRACING_ENABLE_OTLP_HTTP=1)
    target_link_libraries(tracing PUBLIC ${_galay_tracing_http_target})
endif()

set_target_properties(tracing PROPERTIES EXPORT_NAME tracing)

if(GALAY_TRACING_ENABLE_SPDLOG)
    galay_ensure_spdlog()

    add_library(galay-tracing-spdlog adapters/spdlog_sink.cc)
    add_library(galay-tracing::galay-tracing-spdlog ALIAS galay-tracing-spdlog)
    target_compile_features(galay-tracing-spdlog PUBLIC cxx_std_23)
    target_include_directories(galay-tracing-spdlog
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/include>
            $<INSTALL_INTERFACE:include>
    )
    target_link_libraries(galay-tracing-spdlog PUBLIC tracing spdlog::spdlog)
    set_target_properties(galay-tracing-spdlog PROPERTIES EXPORT_NAME tracing-spdlog)
endif()

if(GALAY_TRACING_ENABLE_KERNEL)
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
endif()
