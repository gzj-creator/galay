# galay-tracing Enterprise Design

## Goal

`galay-tracing` provides a production-grade tracing and logging correlation library for the Galay C++ ecosystem. Its first-class goal is to let business code write logs normally while every log record automatically carries `trace_id` and `span_id` when a trace context exists.

The library must also keep a real span pipeline, so logs can be correlated with distributed traces instead of becoming the only tracing data source.

## Non-Goals

- Do not build the tracing core on top of `spdlog`.
- Do not rely on `spdlog` MDC for propagation, because MDC is thread-local and unsuitable for async logger handoff and coroutine migration.
- Do not force OpenTelemetry C++ SDK into the hot path.
- Do not require business code to pass trace identifiers through every function.

## Project and Naming Style

The repository follows the existing `galay-*` style:

- Repository: `galay-tracing`
- CMake project: `galay-tracing`
- Primary target: `galay-tracing`
- Optional adapter target: `galay-tracing-spdlog`
- Optional Galay runtime adapter target: `galay-tracing-kernel`
- Module target: `galay-tracing-modules`
- CMake export namespace: `galay-tracing::`
- Package config files:
  - `galay-tracing-config.cmake`
  - `galay-tracing-config-version.cmake`
  - `galay-tracing-targets.cmake`
- Public include root: `galay-tracing/`
- C++ namespace: `galay::tracing`
- C++ module facade: `galay.tracing`

File names use lower snake case, matching nearby repositories such as `rpc_server.h`, `http_reader.h`, and `timer_scheduler.h`.

Class and type names use PascalCase:

- `TraceId`
- `SpanId`
- `TraceContext`
- `Span`
- `SpanGuard`
- `Logger`
- `LogRecord`
- `BatchSpanProcessor`
- `FileSpanExporter`
- `SpdlogSink`

Functions use camelCase:

- `startSpan(...)`
- `currentContext()`
- `setCurrentContext(...)`
- `extractTraceparent(...)`
- `injectTraceparent(...)`
- `logInfo(...)`
- `forceFlush(...)`

Member variables use the existing `m_` style:

- `m_config`
- `m_context`
- `m_processor`
- `m_sinks`

Constants use the existing `k` prefix:

- `kTraceIdBytes`
- `kSpanIdBytes`
- `kDefaultQueueCapacity`
- `kDefaultBatchSize`

Configuration struct fields use snake_case, matching config structs in `galay-http` and `galay-rpc`:

- `queue_capacity`
- `batch_size`
- `flush_interval_ms`
- `drop_debug_when_full`

## Directory Layout

```text
galay-tracing/
  common/
    trace_id.h
    span_id.h
    source_location.h
  context/
    trace_context.h
    context_storage.h
    traceparent.h
    baggage.h
  kernel/
    span.h
    span_guard.h
    sampler.h
    span_processor.h
    batch_span_processor.h
    span_exporter.h
  log/
    log_level.h
    log_record.h
    logger.h
    logger_registry.h
    log_sink.h
    console_sink.h
    file_sink.h
  adapters/
    spdlog_sink.h
    kernel_context.h
  module/
    module_prelude.hpp
    galay_tracing.cppm
```

Supporting trees:

```text
examples/include/
examples/import/
test/
benchmark/
cmake/
docs/
```

## Architecture

The design has four layers.

1. Core identifiers and context.

   `TraceId`, `SpanId`, and `TraceContext` are compact binary types. They format to lowercase hex only when needed for logs, headers, or export. `TraceContext` stores `trace_id`, `span_id`, optional `parent_span_id`, trace flags, and sampled state.

2. Span lifecycle.

   `Span` records operation name, start/end timestamps, status, attributes, and bounded events. `SpanGuard` is an RAII helper that sets the current context on construction and restores the previous context on destruction. Unsampled spans keep propagation context but avoid attribute/event allocation.

3. Log facade.

   `Logger` creates `LogRecord` objects. Each record snapshots the current `TraceContext` at the call site before any async handoff. Business code uses macros such as `GALAY_LOG_INFO(...)` so source location is captured without manual arguments.

4. Export pipeline.

   Span export uses a bounded queue and a background batch processor. The first production exporter is `FileSpanExporter` with JSON lines output for local validation. OTLP exporter is planned as a follow-up adapter to avoid forcing protobuf or gRPC into the first core build.

## Logging API

Primary business-facing macros:

```cpp
GALAY_LOG_TRACE("message {}", value);
GALAY_LOG_DEBUG("message {}", value);
GALAY_LOG_INFO("message {}", value);
GALAY_LOG_WARN("message {}", value);
GALAY_LOG_ERROR("message {}", value);
```

Typed API behind the macros:

```cpp
galay::tracing::logInfo("order accepted, order_id={}", order_id);
galay::tracing::logError("rpc timeout, peer={}", peer);
```

Example output:

```text
ts=2026-05-19T11:30:21.123Z level=INFO trace_id=4bf92f3577b34da6a3ce929d0e0e4736 span_id=00f067aa0ba902b7 msg="order accepted, order_id=123"
```

When no trace context exists, logging remains valid and omits trace fields or prints zero identifiers according to `LoggerConfig`.

## Trace Context Propagation

The library must support W3C Trace Context:

- Extract inbound `traceparent`.
- Inject outbound `traceparent`.
- Preserve `tracestate` as an opaque string in the first version.

The core parser must reject malformed identifiers, all-zero trace ids, and all-zero span ids.

HTTP/RPC integrations should call:

```cpp
auto context = galay::tracing::extractTraceparent(headers);
auto span = galay::tracing::startServerSpan("POST /orders", context);
galay::tracing::injectTraceparent(out_headers, galay::tracing::currentContext());
```

## Galay Kernel Integration

`galay-kernel` uses C++23 coroutines and multiple schedulers, so plain `thread_local` is not sufficient as the long-term propagation model.

The first version uses `thread_local` storage as the fallback context store and provides a narrow adapter in `galay-tracing/adapters/kernel_context.h`. The adapter is responsible for capturing the current context before scheduling and restoring it when a task resumes.

The target split keeps this dependency explicit:

- `galay-tracing` has no dependency on `galay-kernel`.
- `galay-tracing-kernel` links `galay-kernel::galay-kernel`.
- Future `galay-http` or `galay-rpc` middleware should depend on `galay-tracing-kernel` only when they need coroutine-aware propagation.

## Spdlog Integration

`spdlog` is a sink adapter, not the tracing core.

`SpdlogSink` receives `LogRecord` objects where `trace_id` and `span_id` have already been snapshotted. This preserves correctness when `spdlog` async logging is enabled.

The adapter target is optional:

```cmake
option(GALAY_TRACING_ENABLE_SPDLOG "Build spdlog sink adapter" ON)
find_package(spdlog CONFIG)
```

If `spdlog` is not found and the option is enabled, configuration fails with a clear message. If disabled, the core still builds with console and file sinks.

## CMake and Install Design

Root `CMakeLists.txt` follows the Galay style:

- `cmake_minimum_required(VERSION 3.16)`
- `project(galay-tracing VERSION 0.1.0 LANGUAGES CXX)`
- `CMAKE_CXX_STANDARD 23`
- `BUILD_TESTING` defaults to `OFF`
- `BUILD_TESTS` is a deprecated compatibility alias
- `BUILD_EXAMPLES` defaults to `ON`
- `BUILD_BENCHMARKS` defaults to `ON`
- module support is gated by CMake version, generator, and compiler

Installed targets:

```text
galay-tracing::galay-tracing
galay-tracing::galay-tracing-spdlog
galay-tracing::galay-tracing-kernel
galay-tracing::galay-tracing-modules
```

The package config must call `find_dependency(galay-kernel 4.0.0 CONFIG)` only when the kernel adapter target is exported, and `find_dependency(spdlog CONFIG)` only when the spdlog adapter target is exported.

Headers install to:

```text
include/galay-tracing/...
```

Package files install to:

```text
lib/cmake/galay-tracing/
```

## Performance Rules

Hot-path logging:

- Check log level before formatting.
- Snapshot current context at the call site.
- Avoid heap allocation for disabled logs.
- Do not call `spdlog` MDC from the hot path.
- Keep source location capture macro-based.

Hot-path spans:

- Sampling decision is made at span start.
- Unsampled spans avoid attribute and event allocation.
- IDs are binary in memory and formatted lazily.
- `SpanGuard` must be noexcept on destruction.

Queues:

- Log and span queues are bounded.
- Queue-full policy is configurable.
- Default log policy drops `TRACE` and `DEBUG` first, keeps `WARN` and `ERROR` best-effort.
- Default span policy drops spans and increments counters instead of blocking scheduler threads.

Shutdown:

- `forceFlush(timeout)` drains queues up to the timeout.
- `shutdown(timeout)` stops background workers and drains best-effort.
- Destructors must not block indefinitely.

## Error Handling

Configuration errors fail early through CMake or explicit `std::expected` return values.

Runtime exporter errors are not thrown through business code paths. They are counted in internal metrics and optionally emitted through an internal diagnostics sink.

Malformed inbound trace headers do not fail the request. They start a new root trace and increment a parse error counter.

## Testing Strategy

Core tests:

- Trace id and span id parse/format.
- `traceparent` extract/inject.
- `SpanGuard` context restore.
- Nested spans.
- No-context logging.
- Context logging.
- Disabled log level avoids formatting.

Concurrency tests:

- Multi-thread logging snapshots the right context.
- Bounded queue full policy drops expected records.
- `forceFlush(timeout)` completes.
- Kernel adapter preserves context across scheduled tasks.

Consumer tests:

- `find_package(galay-tracing CONFIG REQUIRED)`.
- Link against `galay-tracing::galay-tracing`.
- Link against optional adapter targets when enabled.

Benchmark targets:

- Disabled log overhead.
- Enabled log enqueue overhead.
- Span start/end sampled.
- Span start/end unsampled.
- Traceparent parse/inject.
- Async queue throughput.

## First Release Scope

Version `0.1.0` should include:

- Core trace identifiers.
- Trace context storage.
- W3C `traceparent` parse/inject.
- Span and `SpanGuard`.
- Always-on and ratio sampler.
- Logger facade and macros.
- Console/file sink.
- Optional `SpdlogSink`.
- Batch span processor.
- JSON-lines file span exporter.
- CMake install/export package.
- Include examples, tests, and benchmarks.

Deferred:

- OTLP/protobuf exporter.
- Full baggage semantics.
- Automatic HTTP/RPC middleware patches in sibling repositories.
- Metrics pipeline.
- Remote dynamic sampling.
