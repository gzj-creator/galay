add_executable(T1-package_surface t1_package_surface.cc)
target_link_libraries(T1-package_surface PRIVATE galay-tracing::galay-tracing)

add_test(NAME T1-package_surface COMMAND T1-package_surface)

add_executable(T2-trace_ids t2_trace_ids.cc)
target_link_libraries(T2-trace_ids PRIVATE galay-tracing::galay-tracing)

add_test(NAME T2-trace_ids COMMAND T2-trace_ids)

add_executable(T3-traceparent t3_traceparent.cc)
target_link_libraries(T3-traceparent PRIVATE galay-tracing::galay-tracing)

add_test(NAME T3-traceparent COMMAND T3-traceparent)

add_executable(T4-span_guard t4_span_guard.cc)
target_link_libraries(T4-span_guard PRIVATE galay-tracing::galay-tracing)

add_test(NAME T4-span_guard COMMAND T4-span_guard)

add_executable(T5-logger_context t5_logger_context.cc)
target_link_libraries(T5-logger_context PRIVATE galay-tracing::galay-tracing)

add_test(NAME T5-logger_context COMMAND T5-logger_context)

if(GALAY_TRACING_ENABLE_SPDLOG)
    add_executable(T6-spdlog_sink t6_spdlog_sink.cc)
    target_link_libraries(T6-spdlog_sink PRIVATE galay-tracing::galay-tracing-spdlog)

    add_test(NAME T6-spdlog_sink COMMAND T6-spdlog_sink)
endif()

add_executable(T7-batch_processor t7_batch_processor.cc)
target_link_libraries(T7-batch_processor PRIVATE galay-tracing::galay-tracing)

add_test(NAME T7-batch_processor COMMAND T7-batch_processor)

add_executable(T9-otlp_http_exporter t9_otlp_http_exporter.cc)
target_link_libraries(T9-otlp_http_exporter PRIVATE galay-tracing::galay-tracing)

add_test(NAME T9-otlp_http_exporter COMMAND T9-otlp_http_exporter)

add_executable(T10-base_logger t10_base_logger.cc)
target_link_libraries(T10-base_logger PRIVATE galay-tracing::galay-tracing)

add_test(NAME T10-base_logger COMMAND T10-base_logger)

if(GALAY_TRACING_ENABLE_KERNEL)
    add_executable(T8-kernel_context t8_kernel_context.cc)
    target_link_libraries(T8-kernel_context PRIVATE galay-tracing::galay-tracing-kernel)

    add_test(NAME T8-kernel_context COMMAND T8-kernel_context)
endif()
