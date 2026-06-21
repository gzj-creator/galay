function(galay_add_tracing_test_include target_name)
    target_include_directories(${target_name} PRIVATE ${CMAKE_BINARY_DIR}/include)
endfunction()

add_executable(T1-package_surface t1_package_surface.cc)
galay_add_tracing_test_include(T1-package_surface)
target_link_libraries(T1-package_surface PRIVATE galay-tracing::galay-tracing)

add_test(NAME tracing.package_surface COMMAND T1-package_surface)

add_executable(T2-trace_ids t2_trace_ids.cc)
galay_add_tracing_test_include(T2-trace_ids)
target_link_libraries(T2-trace_ids PRIVATE galay-tracing::galay-tracing)

add_test(NAME tracing.trace_ids COMMAND T2-trace_ids)

add_executable(T3-traceparent t3_traceparent.cc)
galay_add_tracing_test_include(T3-traceparent)
target_link_libraries(T3-traceparent PRIVATE galay-tracing::galay-tracing)

add_test(NAME tracing.traceparent COMMAND T3-traceparent)

add_executable(T4-span_guard t4_span_guard.cc)
galay_add_tracing_test_include(T4-span_guard)
target_link_libraries(T4-span_guard PRIVATE galay-tracing::galay-tracing)

add_test(NAME tracing.span_guard COMMAND T4-span_guard)

add_executable(T5-logger_context t5_logger_context.cc)
galay_add_tracing_test_include(T5-logger_context)
target_link_libraries(T5-logger_context PRIVATE galay-tracing::galay-tracing)

add_test(NAME tracing.logger_context COMMAND T5-logger_context)

if(GALAY_TRACING_ENABLE_SPDLOG)
    add_executable(T6-spdlog_sink t6_spdlog_sink.cc)
    galay_add_tracing_test_include(T6-spdlog_sink)
    target_link_libraries(T6-spdlog_sink PRIVATE galay-tracing::galay-tracing-spdlog)

    add_test(NAME tracing.spdlog_sink COMMAND T6-spdlog_sink)
endif()

add_executable(T7-batch_processor t7_batch_processor.cc)
galay_add_tracing_test_include(T7-batch_processor)
target_link_libraries(T7-batch_processor PRIVATE galay-tracing::galay-tracing)

add_test(NAME tracing.batch_processor COMMAND T7-batch_processor)

add_executable(T9-otlp_http_exporter t9_otlp_http_exporter.cc)
galay_add_tracing_test_include(T9-otlp_http_exporter)
target_link_libraries(T9-otlp_http_exporter PRIVATE galay-tracing::galay-tracing)

add_test(NAME tracing.otlp_http_exporter COMMAND T9-otlp_http_exporter)

add_executable(T10-base_logger t10_base_logger.cc)
galay_add_tracing_test_include(T10-base_logger)
target_link_libraries(T10-base_logger PRIVATE galay-tracing::galay-tracing)

add_test(NAME tracing.base_logger COMMAND T10-base_logger)

add_executable(T11-http_headers t11_http_headers.cc)
galay_add_tracing_test_include(T11-http_headers)
target_link_libraries(T11-http_headers PRIVATE galay-tracing::galay-tracing)

add_test(NAME tracing.http_headers COMMAND T11-http_headers)

add_executable(T12-span_events_links t12_span_events_links.cc)
galay_add_tracing_test_include(T12-span_events_links)
target_link_libraries(T12-span_events_links PRIVATE galay-tracing::galay-tracing)

add_test(NAME tracing.span_events_links COMMAND T12-span_events_links)

add_executable(T13-tracer_provider t13_tracer_provider.cc)
galay_add_tracing_test_include(T13-tracer_provider)
target_link_libraries(T13-tracer_provider PRIVATE galay-tracing::galay-tracing)

add_test(NAME tracing.tracer_provider COMMAND T13-tracer_provider)

add_executable(T8-kernel_context t8_kernel_context.cc)
galay_add_tracing_test_include(T8-kernel_context)
target_link_libraries(T8-kernel_context PRIVATE galay-tracing::galay-tracing-kernel)

add_test(NAME tracing.kernel_context COMMAND T8-kernel_context)
