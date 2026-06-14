# Tracing Docs And Gap Closure Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Make `galay-tracing` understandable for production users now, then close tracing feature gaps in small TDD batches.

**Architecture:** Treat existing public headers and tests as the source of truth. First document the currently supported ingestion, propagation, logging, exporting, coroutine, and performance paths. Then add missing tracing semantics such as span kind/status/attributes/resource/sampling integration as separate code tasks with tests.

**Tech Stack:** C++23, CMake, CTest, optional `galay-http`, optional `galay-kernel`, optional C++ modules.

---

### Task 1: Public Documentation Baseline

**Files:**
- Modify: `README.md`
- Create: `docs/01-快速开始.md`
- Create: `docs/02-链路上下文.md`
- Create: `docs/03-日志关联.md`
- Create: `docs/04-导出到Collector.md`
- Create: `docs/06-功能边界与路线图.md`

**Steps:**
1. Replace the skeleton README with current build, feature, and document links.
2. Document the minimal include-based flow: start span, log, extract/inject `traceparent`.
3. Document context propagation rules: TLS fallback, explicit context in coroutines, inbound server spans.
4. Document log correlation: default logger, custom writer, structured event macros, disabled field short-circuiting.
5. Document OTLP/HTTP JSON exporter configuration, custom transport, collector endpoint, and scheduler-thread blocking guard.
6. Document known missing features and the order they should be implemented.
7. Verify examples referenced by docs compile.

### Task 2: Production-Shaped Examples

**Files:**
- Modify: `examples/CMakeLists.txt`
- Create: `examples/include/e3_otlp_exporter.cc`
- Create: `examples/import/e3_otlp_exporter.cc`

**Steps:**
1. Add an include example that creates an `OtlpHttpExporter` with configurable endpoint/headers and a mock transport.
2. Add a matching import example using the module facade when module support is enabled.
3. Register both examples in `examples/CMakeLists.txt`.
4. Build `build-release` with examples enabled and run the include example.

### Task 3: Span Semantic Model

**Files:**
- Modify: `galay-tracing/kernel/span.h`
- Modify: `galay-tracing/kernel/span.cc`
- Modify: `galay-tracing/kernel/otlp_http_exporter.cc`
- Modify: `test/t4_span_guard.cc`
- Modify: `test/t9_otlp_http_exporter.cc`

**Steps:**
1. Write failing tests for span kind, status code/message, and bounded attributes.
2. Add compact `SpanKind`, `SpanStatus`, and `SpanAttribute` APIs without adding allocation to unsampled/default hot paths unnecessarily.
3. Encode these fields in OTLP JSON.
4. Update module exports and docs.
5. Run targeted tests, then full CTest.

### Task 4: Sampling Integration

**Files:**
- Modify: `galay-tracing/kernel/sampler.h`
- Modify: `galay-tracing/kernel/sampler.cc`
- Modify: `galay-tracing/kernel/span_guard.h`
- Modify: `galay-tracing/kernel/span_guard.cc`
- Modify: `test/t4_span_guard.cc`

**Steps:**
1. Write failing tests proving parent sampled flag propagation and configured sampler decisions.
2. Add a process-wide sampler hook or explicit tracer configuration without changing the simple `startSpan()` API.
3. Add parent-based and ratio sampler only if the API remains small and deterministic.
4. Verify sampled spans export and unsampled spans skip processor enqueue.

### Task 5: HTTP Middleware And Propagation Helpers

**Files:**
- Create or modify under `galay-tracing/adapters/` only after checking `galay-http` public APIs.
- Add tests or examples using mock request/response header maps.

**Steps:**
1. Write failing tests for inbound extract and outbound inject helpers.
2. Implement helpers over generic header get/set concepts, not a hard dependency unless needed.
3. Document how services use these helpers at boundaries.

### Verification

Run after each completed task:

```bash
rtk git diff --check
rtk cmake --build build-dev
rtk ctest --test-dir build-dev --output-on-failure
```

Run before handoff:

```bash
rtk cmake --build build-release && rtk ctest --test-dir build-release --output-on-failure
rtk cmake --build build-dev && rtk ctest --test-dir build-dev --output-on-failure
rtk cmake --build build-no-http && rtk ctest --test-dir build-no-http --output-on-failure
```
