# galay-tracing Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Build the first production-shaped `galay-tracing` C++23 library with automatic `trace_id/span_id` logging correlation, W3C trace context support, span lifecycle tracking, optional `spdlog` sink, optional `galay-kernel` adapter, CMake install exports, tests, examples, and benchmarks.

**Architecture:** Keep the hot path in `galay-tracing` independent from `spdlog` and `galay-kernel`. Snapshot trace context at the log call site, use RAII `SpanGuard` for context scope, and move span export to bounded background batch processing.

**Tech Stack:** C++23, CMake package exports, CTest, optional `spdlog`, optional `galay-kernel::galay-kernel`, optional C++23 module facade.

---

### Task 1: Repository Skeleton and CMake Package

**Files:**
- Create: `CMakeLists.txt`
- Create: `galay-tracing/CMakeLists.txt`
- Create: `cmake/option.cmake`
- Create: `cmake/galay-tracing-config.cmake.in`
- Create: `README.md`

**Step 1: Write the failing package smoke test**

Create `test/t1_package_surface.cc`:

```cpp
#include "galay-tracing/common/trace_id.h"

int main() {
    auto id = galay::tracing::TraceId::zero();
    return id.isValid() ? 1 : 0;
}
```

**Step 2: Configure and verify it fails**

Run:

```bash
cmake -S . -B build-dev -DBUILD_TESTING=ON -DBUILD_EXAMPLES=OFF -DBUILD_BENCHMARKS=OFF
```

Expected: configuration fails because the project and header do not exist yet.

**Step 3: Add minimal CMake skeleton**

Implement:

- `project(galay-tracing VERSION 0.1.0 LANGUAGES CXX)`
- C++23 required, extensions off.
- `BUILD_TESTING` defaults to `OFF`.
- `BUILD_TESTS` compatibility alias.
- `BUILD_EXAMPLES`, `BUILD_BENCHMARKS`, `GALAY_TRACING_ENABLE_SPDLOG`, `GALAY_TRACING_ENABLE_KERNEL`, `ENABLE_CPP23_MODULES`.
- Primary shared or static library target `galay-tracing`.
- Install export `galay-tracing-targets.cmake` with namespace `galay-tracing::`.
- Config files under `lib/cmake/galay-tracing`.

**Step 4: Run configure**

Run:

```bash
cmake -S . -B build-dev -DBUILD_TESTING=ON -DBUILD_EXAMPLES=OFF -DBUILD_BENCHMARKS=OFF
```

Expected: configuration succeeds or fails only because Task 2 headers are still missing.

**Step 5: Commit**

```bash
git add CMakeLists.txt galay-tracing/CMakeLists.txt cmake README.md test/t1_package_surface.cc
git commit -m "feat: add galay-tracing project skeleton"
```

### Task 2: Trace Identifier Types

**Files:**
- Create: `galay-tracing/common/trace_id.h`
- Create: `galay-tracing/common/span_id.h`
- Create: `galay-tracing/common/id_format.h`
- Create: `galay-tracing/common/id_format.cc`
- Create: `test/t2_trace_ids.cc`

**Step 1: Write failing tests**

Test:

- `TraceId::fromHex(...)` accepts 32 lowercase hex chars.
- `TraceId::fromHex(...)` rejects malformed and all-zero ids.
- `SpanId::fromHex(...)` accepts 16 lowercase hex chars.
- `toHex()` round trips.
- `TraceId::random()` and `SpanId::random()` do not produce zero.

**Step 2: Run test to verify it fails**

Run:

```bash
cmake --build build-dev --target T2-trace_ids
```

Expected: compile fails because identifier types are missing.

**Step 3: Implement identifier types**

Use fixed-size `std::array<std::byte, 16>` for `TraceId` and `std::array<std::byte, 8>` for `SpanId`. Keep formatting allocation-free when caller provides an output buffer; allow `std::string toHex() const` for convenience.

**Step 4: Run tests**

Run:

```bash
cmake --build build-dev --target T2-trace_ids
./build-dev/bin/T2-trace_ids
```

Expected: PASS.

**Step 5: Commit**

```bash
git add galay-tracing/common test/t2_trace_ids.cc
git commit -m "feat: add trace and span identifier types"
```

### Task 3: Trace Context and W3C Traceparent

**Files:**
- Create: `galay-tracing/context/trace_context.h`
- Create: `galay-tracing/context/traceparent.h`
- Create: `galay-tracing/context/traceparent.cc`
- Create: `test/t3_traceparent.cc`

**Step 1: Write failing tests**

Cover valid extraction, injection, rejected all-zero trace id, rejected all-zero span id, rejected wrong version, rejected bad flags, and opaque `tracestate` preservation.

**Step 2: Run test to verify it fails**

Run:

```bash
cmake --build build-dev --target T3-traceparent
```

Expected: compile fails because parser is missing.

**Step 3: Implement parser and injector**

Expose:

```cpp
std::expected<TraceContext, TraceparentError> extractTraceparent(std::string_view value);
std::string injectTraceparent(const TraceContext& context);
```

Do not throw on malformed input.

**Step 4: Run tests**

Run:

```bash
cmake --build build-dev --target T3-traceparent
./build-dev/bin/T3-traceparent
```

Expected: PASS.

**Step 5: Commit**

```bash
git add galay-tracing/context test/t3_traceparent.cc
git commit -m "feat: add W3C traceparent support"
```

### Task 4: Context Storage and SpanGuard

**Files:**
- Create: `galay-tracing/context/context_storage.h`
- Create: `galay-tracing/context/context_storage.cc`
- Create: `galay-tracing/kernel/span.h`
- Create: `galay-tracing/kernel/span.cc`
- Create: `galay-tracing/kernel/span_guard.h`
- Create: `galay-tracing/kernel/span_guard.cc`
- Create: `test/t4_span_guard.cc`

**Step 1: Write failing tests**

Cover root span, nested span, restore previous context, move-only guard behavior, and no-throw destruction.

**Step 2: Run test to verify it fails**

Run:

```bash
cmake --build build-dev --target T4-span_guard
```

Expected: compile fails because span APIs are missing.

**Step 3: Implement minimal context and span lifecycle**

Use `thread_local` fallback storage in the core target. Make `SpanGuard` own the active span and previous context. Expose:

```cpp
std::optional<TraceContext> currentContext() noexcept;
SpanGuard startSpan(std::string_view name);
SpanGuard startServerSpan(std::string_view name, const TraceContext& parent);
```

**Step 4: Run tests**

Run:

```bash
cmake --build build-dev --target T4-span_guard
./build-dev/bin/T4-span_guard
```

Expected: PASS.

**Step 5: Commit**

```bash
git add galay-tracing/context galay-tracing/kernel test/t4_span_guard.cc
git commit -m "feat: add span context scopes"
```

### Task 5: Logger Facade and Sinks

**Files:**
- Create: `galay-tracing/common/source_location.h`
- Create: `galay-tracing/log/log_level.h`
- Create: `galay-tracing/log/log_record.h`
- Create: `galay-tracing/log/log_sink.h`
- Create: `galay-tracing/log/logger.h`
- Create: `galay-tracing/log/logger.cc`
- Create: `galay-tracing/log/console_sink.h`
- Create: `galay-tracing/log/console_sink.cc`
- Create: `test/t5_logger_context.cc`

**Step 1: Write failing tests**

Use a test sink to assert:

- no-context logs still emit.
- context logs include trace and span ids.
- disabled log level does not call formatting.
- source file and line are captured through macros.

**Step 2: Run test to verify it fails**

Run:

```bash
cmake --build build-dev --target T5-logger_context
```

Expected: compile fails because logger facade is missing.

**Step 3: Implement logger facade**

Expose macros:

```cpp
GALAY_LOG_TRACE(...)
GALAY_LOG_DEBUG(...)
GALAY_LOG_INFO(...)
GALAY_LOG_WARN(...)
GALAY_LOG_ERROR(...)
```

Expose typed functions:

```cpp
template<typename... Args>
void logInfo(std::format_string<Args...> fmt, Args&&... args);
```

Check level before formatting.

**Step 4: Run tests**

Run:

```bash
cmake --build build-dev --target T5-logger_context
./build-dev/bin/T5-logger_context
```

Expected: PASS.

**Step 5: Commit**

```bash
git add galay-tracing/common galay-tracing/log test/t5_logger_context.cc
git commit -m "feat: add trace-aware logger facade"
```

### Task 6: Spdlog Sink Adapter

**Files:**
- Create: `galay-tracing/adapters/spdlog_sink.h`
- Create: `galay-tracing/adapters/spdlog_sink.cc`
- Create: `test/t6_spdlog_sink.cc`
- Modify: `CMakeLists.txt`
- Modify: `galay-tracing/CMakeLists.txt`
- Modify: `cmake/galay-tracing-config.cmake.in`

**Step 1: Write failing adapter test**

When `GALAY_TRACING_ENABLE_SPDLOG=ON`, write to a memory or file sink and assert the rendered line contains `trace_id=` and `span_id=`.

**Step 2: Configure with adapter**

Run:

```bash
cmake -S . -B build-spdlog -DBUILD_TESTING=ON -DGALAY_TRACING_ENABLE_SPDLOG=ON
```

Expected: configure fails if `spdlog` is missing, otherwise compile fails because adapter is missing.

**Step 3: Implement `SpdlogSink`**

The sink receives already-snapshotted `LogRecord` values and formats trace fields itself. Do not rely on `spdlog` MDC.

**Step 4: Run tests**

Run:

```bash
cmake --build build-spdlog --target T6-spdlog_sink
./build-spdlog/bin/T6-spdlog_sink
```

Expected: PASS when spdlog is available.

**Step 5: Commit**

```bash
git add galay-tracing/adapters cmake CMakeLists.txt galay-tracing/CMakeLists.txt test/t6_spdlog_sink.cc
git commit -m "feat: add spdlog sink adapter"
```

### Task 7: Batch Span Processor and File Exporter

**Files:**
- Create: `galay-tracing/kernel/sampler.h`
- Create: `galay-tracing/kernel/sampler.cc`
- Create: `galay-tracing/kernel/span_processor.h`
- Create: `galay-tracing/kernel/batch_span_processor.h`
- Create: `galay-tracing/kernel/batch_span_processor.cc`
- Create: `galay-tracing/kernel/span_exporter.h`
- Create: `galay-tracing/kernel/file_span_exporter.h`
- Create: `galay-tracing/kernel/file_span_exporter.cc`
- Create: `test/t7_batch_processor.cc`

**Step 1: Write failing tests**

Cover sampled export, unsampled no-export, queue full drop count, `forceFlush(timeout)`, and `shutdown(timeout)`.

**Step 2: Run test to verify it fails**

Run:

```bash
cmake --build build-dev --target T7-batch_processor
```

Expected: compile fails because processor is missing.

**Step 3: Implement processor and exporter**

Use a bounded queue, condition variable, batch size, flush interval, and shutdown flag. Destructors must not block indefinitely.

**Step 4: Run tests**

Run:

```bash
cmake --build build-dev --target T7-batch_processor
./build-dev/bin/T7-batch_processor
```

Expected: PASS.

**Step 5: Commit**

```bash
git add galay-tracing/kernel test/t7_batch_processor.cc
git commit -m "feat: add batched span export pipeline"
```

### Task 8: Galay Kernel Context Adapter

**Files:**
- Create: `galay-tracing/adapters/kernel_context.h`
- Create: `test/t8_kernel_context.cc`
- Modify: `CMakeLists.txt`
- Modify: `galay-tracing/CMakeLists.txt`
- Modify: `cmake/galay-tracing-config.cmake.in`

**Step 1: Write failing kernel propagation test**

Schedule a coroutine task through `galay-kernel`, log inside the task, and assert the log record carries the parent trace context.

**Step 2: Configure with kernel adapter**

Run:

```bash
cmake -S . -B build-kernel -DBUILD_TESTING=ON -DGALAY_TRACING_ENABLE_KERNEL=ON
```

Expected: configure finds `galay-kernel::galay-kernel` or fails with a clear dependency error.

**Step 3: Implement adapter**

Provide a minimal wrapper that captures `currentContext()` before task submission and restores it around coroutine/task execution.

**Step 4: Run tests**

Run:

```bash
cmake --build build-kernel --target T8-kernel_context
./build-kernel/bin/T8-kernel_context
```

Expected: PASS.

**Step 5: Commit**

```bash
git add galay-tracing/adapters cmake CMakeLists.txt galay-tracing/CMakeLists.txt test/t8_kernel_context.cc
git commit -m "feat: add galay-kernel context adapter"
```

### Task 9: Examples and Module Facade

**Files:**
- Create: `examples/CMakeLists.txt`
- Create: `examples/include/e1_log_context.cc`
- Create: `examples/include/e2_traceparent.cc`
- Create: `examples/import/e1_log_context.cc`
- Create: `examples/import/e2_traceparent.cc`
- Create: `galay-tracing/module/module_prelude.hpp`
- Create: `galay-tracing/module/galay_tracing.cppm`

**Step 1: Write examples**

Include examples must show:

- root span with automatic log correlation.
- inbound `traceparent` extraction and outbound injection.

Import examples must mirror the include examples when module support is effective.

**Step 2: Build examples**

Run:

```bash
cmake -S . -B build-examples -DBUILD_EXAMPLES=ON -DBUILD_TESTING=OFF
cmake --build build-examples --parallel
```

Expected: include examples build. Import examples build only when module support is effective.

**Step 3: Run examples**

Run:

```bash
./build-examples/bin/E1-LogContext
./build-examples/bin/E2-Traceparent
```

Expected: output contains trace and span fields.

**Step 4: Commit**

```bash
git add examples galay-tracing/module
git commit -m "feat: add examples and module facade"
```

### Task 10: Benchmarks and Final Verification

**Files:**
- Create: `benchmark/CMakeLists.txt`
- Create: `benchmark/b1_disabled_log.cc`
- Create: `benchmark/b2_enabled_log.cc`
- Create: `benchmark/b3_span_scope.cc`
- Create: `benchmark/b4_traceparent.cc`
- Create: `docs/05-性能测试.md`

**Step 1: Implement benchmark programs**

Measure:

- disabled log overhead.
- enabled log enqueue or sink overhead.
- sampled and unsampled span scope overhead.
- traceparent parse/inject throughput.

**Step 2: Build full tree**

Run:

```bash
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DBUILD_EXAMPLES=ON -DBUILD_BENCHMARKS=ON
cmake --build build-release --parallel
```

Expected: build succeeds.

**Step 3: Run CTest**

Run:

```bash
ctest --test-dir build-release --output-on-failure
```

Expected: all tests pass.

**Step 4: Run benchmarks**

Run:

```bash
./build-release/bin/B1-DisabledLog
./build-release/bin/B2-EnabledLog
./build-release/bin/B3-SpanScope
./build-release/bin/B4-Traceparent
```

Expected: each benchmark prints workload, build type, backend/adapters, and headline throughput or latency.

**Step 5: Install and consumer smoke**

Run:

```bash
cmake --install build-release --prefix install-local
```

Create a temporary consumer that runs:

```cmake
find_package(galay-tracing CONFIG REQUIRED)
target_link_libraries(consumer PRIVATE galay-tracing::galay-tracing)
```

Expected: consumer configures, builds, and links.

**Step 6: Commit**

```bash
git add benchmark docs/05-性能测试.md
git commit -m "test: add benchmarks and release verification notes"
```
