# Tracing C API

`galay-tracing-c` wraps the C++ tracing data model behind opaque C handles.

## Handles And Ownership

- `galay_tracing_trace_context_t` owns a C++ `TraceContext`; destroy it with `galay_tracing_trace_context_destroy`.
- `galay_tracing_provider_t` owns optional processor/export state; destroy it with `galay_tracing_provider_destroy`.
- `galay_tracing_tracer_t`, `galay_tracing_span_t`, `galay_tracing_sampler_t`, and `galay_tracing_logger_t` are owned handles with matching destroy functions.
- Spans borrow their provider. Keep the provider alive until spans and tracers created from it are destroyed.

Trace/span ID helpers are value-style APIs: `galay_tracing_trace_id_generate`,
`galay_tracing_trace_id_parse`, `galay_tracing_trace_id_format`,
`galay_tracing_span_id_generate`, `galay_tracing_span_id_parse`, and
`galay_tracing_span_id_format`. Format calls write into caller-owned buffers.

## Span Surface

The C span API supports string, signed integer, unsigned integer, double, and bool attributes; status; events; and links. `galay_tracing_span_end` ends the real C++ `Span` and submits it to the provider processor when one is configured.

`galay_tracing_provider_set_file_exporter` installs a synchronous local JSONL exporter. `force_flush` and `shutdown` call the configured processor directly.

## Context Propagation

`galay_tracing_trace_context_extract` and `galay_tracing_trace_context_inject` wrap W3C `traceparent` and `tracestate` extraction/injection. The output buffers are caller-owned and are not NUL-terminated unless the caller provides extra space and writes a terminator.

## Sampler And Logger

Samplers wrap the C++ `AlwaysOnSampler`, `AlwaysOffSampler`, and `TraceIdRatioSampler`.

`galay_tracing_logger_create_file` creates a C++ `Logger` with a synchronous local file sink. This path may block on local file I/O and is intended for tests, examples, and simple embedding rather than hot asynchronous scheduler paths.

## Errors

All public functions return `galay_status_t` except destroy functions. Invalid handles, invalid enum values, missing buffers, and malformed trace context return explicit error codes; no C++ exceptions cross the C ABI.
