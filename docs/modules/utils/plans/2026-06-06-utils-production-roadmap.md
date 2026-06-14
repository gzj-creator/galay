# Utils Production Hardening Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Bring the new LRU cache and adjacent reusable utilities to production quality without moving kernel runtime components into galay-utils.

**Architecture:** Keep `galay-utils` header-only or lightweight by default. Extract only reusable, dependency-light primitives from sibling projects; keep scheduler, coroutine wakeup, thread-owning timer services, and runtime behavior inside `galay-kernel`.

**Tech Stack:** C++23, header-only public utilities, CMake/CTest, optional standalone benchmarks.

---

## Progress Ledger

Complete exactly one checkbox when that plan is fully implemented and verified.

- [x] Plan 1: LRU cache production hardening
- [x] Plan 2: Time utility primitives
- [x] Plan 3: ByteQueueView migration
- [x] Plan 4: RingBuffer utility migration
- [x] Plan 5: Benchmark infrastructure

## Explicit Non-Goals

Do not migrate these `galay-kernel` runtime components into `galay-utils`:

- `TimingWheelTimerManager`
- `ThreadSafeTimerManager`
- `TimerScheduler`
- `TimeoutTimer`

If a future task needs timing-wheel behavior in `galay-utils`, implement a new dependency-light utility core instead of moving these kernel classes.

## Execution Rules

- Use TDD for each plan: write failing tests first, verify red, implement, verify green.
- Keep `galay-utils` independent from `galay-kernel`.
- Do not add internal `std::mutex` to coroutine-facing utilities unless a type is explicitly documented as blocking/thread-safe.
- Update `galay-utils/galay_utils.hpp` and `galay-utils/module/galay_utils.cppm` when adding supported public APIs.
- Run `rtk cmake --build cmake-build-test` and `rtk ctest --test-dir cmake-build-test --output-on-failure` before marking any plan complete.
- Do not commit unless the user explicitly requests a commit.

---

## Plan 1: LRU Cache Production Hardening

**Files:**
- Modify: `galay-utils/cache/lru_cache.hpp`
- Modify: `test/cache/cache_test.cpp`
- Optional Modify: `benchmark/lru_cache_benchmark.cpp`
- Optional Docs: `docs/02-API参考.md`, `docs/03-使用指南.md`

**Scope:**

Improve the existing `LruCache` API while preserving non-thread-safe, no-background-work semantics.

**Implementation Checklist:**

- Add `reserve(size_t)` and `maxLoadFactor(float)` pass-throughs for the internal hash table.
- Add compile-time optional `Stats` counters: hits, misses, inserts, updates, capacity evictions, expired evictions, removes, clears.
- Add `stats()`, `resetStats()`, and `statsEnabled()`; statistics stay disabled by default for capacity-only hot paths.
- Add public `purgeExpired()` so users can explicitly pay TTL cleanup cost at chosen points.
- Add `expireAfterAccess` as an opt-in mode separate from current expire-after-write behavior.
- Add tests for stats, reserve API compile coverage, explicit purge, and expire-after-access.
- Re-run LRU benchmark after changes and compare against the current baseline.

**Acceptance:**

- Existing LRU tests still pass.
- New stats and TTL mode tests pass.
- Pure capacity LRU still avoids `Clock::now()` when no TTL entries exist.

---

## Plan 2: Time Utility Primitives

**Files:**
- Create: `galay-utils/core/time.hpp`
- Modify: `galay-utils/galay_utils.hpp`
- Modify: `galay-utils/module/galay_utils.cppm`
- Modify: `test/core/core_test.cpp`

**Scope:**

Add lightweight time primitives that can be reused by cache, limiter, circuit breaker, and future utility modules.
Also move generic timestamp and formatting helpers out of `System` so time APIs have one public home.

**Implementation Checklist:**

- Add `StopWatch<Clock>` with `elapsed()`, `elapsedMs()`, and `reset()`.
- Add `Deadline<Clock>` with `expired()`, `remaining()`, and `fromNow(duration)`.
- Add `Backoff` with fixed, linear, and exponential progression.
- Add `Time::currentTimeMs/currentTimeUs/currentTimeNs`.
- Add `Time::formatTime/currentGMTTime/currentLocalTime`.
- Remove the corresponding timestamp and formatting APIs from `System`.
- Keep all types non-owning and thread-neutral; no worker thread, no timer scheduler.
- Add deterministic tests using a manual test clock where practical.

**Acceptance:**

- Time utilities compile as standalone headers.
- Tests cover zero/negative durations, expired deadlines, reset behavior, and capped exponential backoff.
- `System` no longer exposes time-related helpers; callers use `Time` instead.

---

## Plan 3: ByteQueueView Migration

**Files:**
- Create: `galay-utils/cache/byte_queue_view.hpp`
- Modify: `galay-utils/galay_utils.hpp`
- Modify: `galay-utils/module/galay_utils.cppm`
- Modify: `test/buffer/buffer_test.cpp`
- Source reference only: `/Users/gongzhijie/Desktop/projects/git/galay-kernel/galay-kernel/common/queue_view.h`

**Scope:**

Move the dependency-light byte queue concept into `galay-utils` as a parser/network utility without depending on kernel runtime code.

**Implementation Checklist:**

- Port `ByteQueueView` into namespace `galay::utils`.
- Keep it header-only and standard-library-only.
- Add `append(std::span<const std::byte>)` if the project accepts `<span>` and `std::byte` in this module.
- Add tests for append, consume, clear, bounds-checked `view`, compaction, and empty append.

**Acceptance:**

- No include path references `galay-kernel`.
- Tests prove compaction does not corrupt readable bytes.

---

## Plan 4: RingBuffer Utility Migration

**Files:**
- Create: `galay-utils/cache/ring_buffer.hpp`
- Optional Create: `galay-utils/tool/ring_buffer_iovec.hpp`
- Modify: `galay-utils/galay_utils.hpp`
- Modify: `galay-utils/module/galay_utils.cppm`
- Modify: `test/buffer/buffer_test.cpp`
- Optional Modify: `benchmark/ring_buffer_benchmark.cpp`
- Source reference only: `/Users/gongzhijie/Desktop/projects/git/galay-kernel/galay-kernel/common/buffer.h`

**Scope:**

Create a utils-owned ring buffer based on the useful behavior from `galay-kernel`, with portable span views and POSIX I/O views guarded by platform macros.

**Implementation Checklist:**

- Implement `RingBuffer` with fixed capacity, move-only ownership, `readable()`, `writable()`, `capacity()`, `empty()`, and `full()`.
- Add `write`, `read`, `produce`, `consume`, `readSpans`, and `writeSpans` style APIs using portable span-like views.
- Add POSIX `iovec` helpers behind platform macros so `galay-kernel` can migrate without keeping a compatibility wrapper.
- Add tests for zero capacity rejection, full buffer, empty buffer, wrap-around, move construction, partial read/write, produce/consume bounds, and data integrity.
- Add benchmark coverage after correctness is stable.

**Acceptance:**

- Core `ring_buffer.hpp` builds on non-POSIX platforms.
- POSIX I/O helpers are only visible on platforms that provide `<sys/uio.h>`.

---

## Plan 5: Benchmark Infrastructure

**Files:**
- Modify: `CMakeLists.txt`
- Create: `benchmark/CMakeLists.txt`
- Existing: `benchmark/lru_cache_benchmark.cpp`
- Optional Create: `benchmark/byte_queue_view_benchmark.cpp`
- Optional Create: `benchmark/ring_buffer_benchmark.cpp`
- Optional Create: `benchmark/time_wheel_core_benchmark.cpp` only if a utils-owned timing core is later added

**Scope:**

Make benchmark builds repeatable without becoming part of default user builds.

**Implementation Checklist:**

- Add `BUILD_BENCHMARKS` option defaulting to `OFF`.
- Add `benchmark/lru_cache_benchmark.cpp` as a benchmark target when enabled.
- Add ByteQueueView and RingBuffer benchmark targets after their APIs are stable.
- Keep benchmarks out of CTest by default.
- Document benchmark build and run commands in `docs/05-性能测试.md`.
- Add a short warning that benchmark numbers are machine/configuration dependent.

**Acceptance:**

- Default configure/build behavior is unchanged.
- `-DBUILD_BENCHMARKS=ON` builds benchmark binaries.
- Existing CTest suite still passes.
