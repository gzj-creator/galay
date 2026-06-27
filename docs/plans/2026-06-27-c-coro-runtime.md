# C/C++ Coroutine Runtime Unification Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a first-class high-performance C stackful coroutine runtime that shares the existing galay scheduler, timer, and epoll/io_uring/kqueue backends with C++23 `Task<T>`, without routing every C operation through a freshly spawned C++ `Task<void>`.

**Architecture:** Keep C++ and C coroutine objects separate, but unify their scheduling and I/O wakeup boundary through a low-overhead `ReadyEntry` / `ResumeToken` model. C++ keeps `co_await` and `std::coroutine_handle<>`; C gets `galay_coro_*` stackful APIs returning `C_IOResult` structs. The existing C callback ABI remains compatible and serves as the baseline comparison path.

**Tech Stack:** C++23, C11 ABI wrappers, platform context switch assembly (`x86_64` first, `aarch64` later), CMake, CTest, existing galay kernel runtime, epoll/io_uring/kqueue reactors, benchmark targets under `benchmark/c/kernel` and `benchmark/cpp/kernel`.

---

## Non-Negotiable Requirements

- [ ] Work only inside worktree: `/Users/gongzhijie/Desktop/projects/git/galay/.wroktree/c-coro-runtime`.
- [ ] Do not regress existing C++ `Task<T>` / `co_await` public APIs.
- [ ] Do not make C coroutine APIs spawn a C++ `Task<void>` per I/O operation.
- [ ] Keep old C callback APIs working.
- [ ] C coroutine user APIs return result structs, not callback-only results.
- [ ] Every production change is preceded by a failing test.
- [ ] Every completed task updates this document's checkbox and records exact verification commands.
- [ ] Final examples cover TCP, UDP, AIO, file I/O, and timeout APIs.
- [ ] Final benchmarks report throughput, QPS, latency p50/p90/p99, error count, and compare against the current C callback path that internally spawns C++ coroutines.

## Core Design Decisions

- [ ] Scheduler ready queues store a language-neutral ready item, not only `TaskRef`.
- [ ] C++ ready item resumes `TaskState::m_handle.resume()`.
- [ ] C ready item resumes a `C_CoroTask` using C context switching.
- [ ] I/O completion writes an `IORequest` result slot, then schedules a ready item.
- [ ] `resume()` does not carry business results; resumed coroutine reads its own result slot.
- [ ] epoll/kqueue/io_uring `user_data` points to request/controller state with generation checks, not directly to a user coroutine task.
- [ ] Timeout/cancel/close late events are filtered by generation.

## Proposed Public C Result Shape

```c
typedef enum C_IOResultCode {
    C_IOResultOk,
    C_IOResultEof,
    C_IOResultTimeout,
    C_IOResultCancelled,
    C_IOResultInvalid,
    C_IOResultError
} C_IOResultCode;

typedef struct C_IOResult {
    C_IOResultCode code;
    int sys_errno;
    size_t bytes;
    int64_t value;
    void* ptr;
} C_IOResult;
```

---

## Task 0: Worktree Baseline And Plan Tracking

**Files:**
- Modify: `docs/plans/2026-06-27-c-coro-runtime.md`

**Steps:**
- [x] Run `rtk git status --short` in the worktree.
- [x] Run CMake configure:
  `rtk cmake -S . -B build-coro -DBUILD_TESTING=ON -DGALAY_BUILD_C_API=ON -DGALAY_BUILD_BENCHMARKS=ON -DGALAY_BUILD_EXAMPLES=ON`
- [x] Run a narrow baseline build:
  `rtk cmake --build build-coro --target test_c_kernel_header_smoke t52_taskapi benchmark_c_kernel_tcp_socket_client_throughput`
- [x] Run a narrow baseline test:
  `rtk ctest --test-dir build-coro -R "c\\.kernel\\.header_smoke|kernel\\.taskapi|kernel\\.runtime_expected_src" --output-on-failure`
- [x] Record baseline failures here before implementation if the environment is not clean.

**Verification Log:**
- `rtk git status --short`: clean output.
- `rtk cmake -S . -B build-coro -DBUILD_TESTING=ON -DGALAY_BUILD_C_API=ON -DGALAY_BUILD_BENCHMARKS=ON -DGALAY_BUILD_EXAMPLES=ON`: PASS.
- Initial planned build target names were wrong because CMake generates C targets from scenario names and C API was disabled by default. Reconfigured with `GALAY_BUILD_C_API=ON`.
- `rtk cmake --build build-coro --target test_c_kernel_header_smoke t52_taskapi benchmark_c_kernel_tcp_socket_client_throughput`: PASS.
- `rtk cmake --build build-coro --target t124_runtime_expected_src`: PASS.
- `rtk ctest --test-dir build-coro -R "c\\.kernel\\.header_smoke|kernel\\.taskapi|kernel\\.runtime_expected_src" --output-on-failure`: FAIL only for pre-existing `kernel.runtime_expected_src`, which tries to read missing path `src/c/galay-kernel/galay_kernel.cc`. `c.kernel.header_smoke` and `kernel.taskapi` pass.

**Completion:**
- [x] Completed and verified with baseline failure recorded.

---

## Task 1: Add Source Boundary Tests Against C++ Task Bridge

**Purpose:** Prove new C coroutine APIs do not use the old `runtime->spawn(c_api_...)` path.

**Files:**
- Create: `test/c/kernel/t22_coro_source_boundaries.c`
- Modify: `test/c/kernel/CMakeLists.txt`

**Required tests:**
- [ ] New `galay_coro_*` implementation files do not contain `runtime->spawn(`.
- [ ] New `galay_coro_*` implementation files do not contain `Task<void> c_api_`.
- [ ] Legacy callback files may still contain the old bridge; boundary test is targeted.

**Verification:**
- [ ] RED: `rtk ctest --test-dir build-coro -R t22_coro_source_boundaries --output-on-failure`
- [ ] GREEN: same command passes after target registration.

**Completion:**
- [ ] Completed and verified.

---

## Task 2: Introduce ReadyEntry Without Changing Behavior

**Purpose:** Make scheduler internals able to represent either a C++ task or a C coroutine task while preserving current C++ behavior.

**Files:**
- Modify: `src/cpp/galay-kernel/core/task.h`
- Modify: `src/cpp/galay-kernel/core/task.cc`
- Modify: `src/cpp/galay-kernel/core/scheduler.hpp`
- Modify: `src/cpp/galay-kernel/core/scheduler_core.h`
- Modify: `src/cpp/galay-kernel/core/io_scheduler.hpp`
- Test: `test/cpp/kernel/t135_ready_entry_cpp_compat.cc`
- Benchmark: `benchmark/cpp/kernel/b18_ready_entry_wakeup_latency.cc`

**Design:**
- [ ] Add internal `ReadyEntry { kind, void* state }`.
- [ ] Keep a C++ fast path conversion from `TaskRef` to `ReadyEntry`.
- [ ] Keep `TaskRef` public behavior unchanged.
- [ ] Owner-thread resume still clears `m_queued` and `m_resume_owner_only` for C++ tasks.
- [ ] Avoid `std::function`, heap allocation, and virtual `Coroutine::resume()` on the hot path.

**Tests:**
- [ ] C++ `Task<void>` spawn still completes.
- [ ] `Task<T>` result propagation still works.
- [ ] `co_await Task<T>` still resumes parent.
- [ ] `then()` still schedules continuation.
- [ ] Cross-thread schedule still wakes owner scheduler.

**Verification:**
- [ ] RED: new compat test fails before production changes.
- [ ] GREEN: `rtk ctest --test-dir build-coro -R "t135_ready_entry_cpp_compat|kernel" --output-on-failure`
- [ ] Benchmark builds: `rtk cmake --build build-coro --target benchmark_kernel_ready_entry_wakeup_latency`

**Completion:**
- [ ] Completed and verified.

---

## Task 3: Generalize Waker To ResumeToken

**Purpose:** Let I/O completion schedule either C++ `TaskRef` or C `C_CoroTask` without knowing the language.

**Files:**
- Modify: `src/cpp/galay-kernel/core/waker.h`
- Modify: `src/cpp/galay-kernel/core/waker.cc`
- Modify: `src/cpp/galay-kernel/core/awaitable.h`
- Modify: `src/cpp/galay-kernel/core/io_scheduler.hpp`
- Test: `test/cpp/kernel/t136_resume_token_waker.cc`

**Design:**
- [ ] `Waker(TaskRef)` remains source-compatible.
- [ ] Add internal `Waker(ReadyEntry)` or equivalent constructor.
- [ ] `Waker::wakeUp()` schedules the ready item through owner scheduler.
- [ ] Existing C++ awaitables still obtain scheduler from suspended C++ promise.
- [ ] No public C++ coroutine API changes.

**Tests:**
- [ ] C++ socket/timer awaitables wake through generalized waker.
- [ ] Duplicate wake still coalesces for C++ tasks.
- [ ] Invalid or done tasks are ignored safely.

**Verification:**
- [ ] RED: new generalized waker test fails before implementation.
- [ ] GREEN: `rtk ctest --test-dir build-coro -R "t136_resume_token_waker|kernel" --output-on-failure`

**Completion:**
- [ ] Completed and verified.

---

## Task 4: Add C Coroutine Core

**Purpose:** Add first-class C stackful coroutine task objects and scheduler integration without I/O yet.

**Files:**
- Create: `src/c/galay-kernel-c/coro-c/coro_task_c.h`
- Create: `src/c/galay-kernel-c/coro-c/coro_task_c.cc`
- Create: `src/c/galay-kernel-c/coro-c/coro_context_x86_64.S`
- Create: `src/c/galay-kernel-c/coro-c/coro_context_aarch64.S` if platform support is needed in this pass
- Modify: `src/c/galay-kernel-c/CMakeLists.txt`
- Test: `test/c/kernel/t23_coro_task.c`
- Benchmark: `benchmark/c/kernel/b19_coro_scheduler_wakeup_latency.c`

**C API shape:**
- [ ] `galay_coro_spawn(runtime, entry_fn, arg, options, out_task)`
- [ ] `galay_coro_yield()`
- [ ] `galay_coro_current()`
- [ ] `galay_coro_cancel(task)`
- [ ] `galay_coro_join(task, timeout_ms)`
- [ ] `galay_coro_destroy(task)`

**Runtime rules:**
- [ ] Fixed owner scheduler for each C coroutine.
- [ ] No cross-thread direct resume.
- [ ] Stack allocated from a pool or explicit stack allocator.
- [ ] Guard page when platform supports it.
- [ ] `C_CoroTask` status: ready, running, suspended, done, cancelled.
- [ ] Recoverable failures return typed C result codes.
- [ ] No `abort`, `exit`, or hidden process termination.

**Tests:**
- [ ] Spawn/yield/resume completes.
- [ ] Multiple coroutines round-robin.
- [ ] Cancel suspended coroutine.
- [ ] Destroy after completion.
- [ ] Invalid parameters return errors.

**Verification:**
- [ ] RED: `rtk ctest --test-dir build-coro -R t23_coro_task --output-on-failure`
- [ ] GREEN: same command passes.
- [ ] Benchmark builds: `rtk cmake --build build-coro --target benchmark_c_kernel_coro_scheduler_wakeup_latency`

**Completion:**
- [ ] Completed and verified.

---

## Task 5: Add C Coroutine Wait And Result Struct

**Purpose:** Provide the generic C wait primitive that stores results in request state and returns `C_IOResult` after resume.

**Files:**
- Create: `src/c/galay-kernel-c/coro-c/coro_result_c.h`
- Create: `src/c/galay-kernel-c/coro-c/coro_wait_c.h`
- Create: `src/c/galay-kernel-c/coro-c/coro_wait_c.cc`
- Test: `test/c/kernel/t24_coro_result_wait.c`

**Design:**
- [ ] `C_IOResult` is the public result struct.
- [ ] `galay_coro_wait(request, timeout_ms)` yields current coroutine and returns `C_IOResult`.
- [ ] Resume does not pass business results as parameters.
- [ ] Timeout writes `C_IOResultTimeout`.
- [ ] Cancel writes `C_IOResultCancelled`.
- [ ] Late events are filtered by generation.

**Tests:**
- [ ] Immediate ready request returns without scheduling.
- [ ] Suspended request resumes with OK result.
- [ ] Timeout returns timeout result.
- [ ] Cancel returns cancelled result.
- [ ] Late completion after timeout is ignored.

**Verification:**
- [ ] RED: `rtk ctest --test-dir build-coro -R t24_coro_result_wait --output-on-failure`
- [ ] GREEN: same command passes.

**Completion:**
- [ ] Completed and verified.

---

## Task 6: Direct C Coroutine TCP APIs

**Files:**
- Create: `src/c/galay-kernel-c/async-c/tcp_socket_coro_c.h`
- Create: `src/c/galay-kernel-c/async-c/tcp_socket_coro_c.cc`
- Modify: `src/c/galay-kernel-c/CMakeLists.txt`
- Test: `test/c/kernel/t25_coro_tcp.c`
- Example: `examples/c/kernel/e12_coro_tcp_echo.c`
- Benchmark: `benchmark/c/kernel/b20_coro_tcp_echo_throughput.c`
- Benchmark: `benchmark/c/kernel/b21_coro_tcp_vs_callback.c`

**APIs:**
- [ ] `C_IOResult galay_coro_tcp_accept(listener, out_socket, timeout_ms)`
- [ ] `C_IOResult galay_coro_tcp_connect(socket, host, timeout_ms)`
- [ ] `C_IOResult galay_coro_tcp_recv(socket, buffer, length, timeout_ms)`
- [ ] `C_IOResult galay_coro_tcp_send(socket, buffer, length, timeout_ms)`
- [ ] `C_IOResult galay_coro_tcp_close(socket, timeout_ms)`

**Tests and metrics:**
- [ ] Echo server one connection.
- [ ] Partial recv/send.
- [ ] Close while waiting.
- [ ] Timeout while waiting.
- [ ] Source boundary test confirms no C++ Task bridge.
- [ ] Benchmarks report QPS, throughput MB/s, p50/p90/p99 latency, errors.
- [ ] Compare against old `galay_kernel_tcp_socket_* callback` path.

**Completion:**
- [ ] Completed and verified.

---

## Task 7: Direct C Coroutine UDP APIs

**Files:**
- Create: `src/c/galay-kernel-c/async-c/udp_socket_coro_c.h`
- Create: `src/c/galay-kernel-c/async-c/udp_socket_coro_c.cc`
- Test: `test/c/kernel/t26_coro_udp.c`
- Example: `examples/c/kernel/e13_coro_udp_echo.c`
- Benchmark: `benchmark/c/kernel/b22_coro_udp_throughput.c`
- Benchmark: `benchmark/c/kernel/b23_coro_udp_vs_callback.c`

**APIs:**
- [ ] `C_IOResult galay_coro_udp_recvfrom(socket, buffer, length, out_host, timeout_ms)`
- [ ] `C_IOResult galay_coro_udp_sendto(socket, buffer, length, host, timeout_ms)`

**Tests and metrics:**
- [ ] Echo round trip.
- [ ] Timeout.
- [ ] Packet loss/error count in benchmark.
- [ ] QPS, throughput MB/s, p50/p90/p99 latency.
- [ ] Compare against existing UDP callback benchmark path.

**Completion:**
- [ ] Completed and verified.

---

## Task 8: Direct C Coroutine File I/O And AIO APIs

**Files:**
- Create: `src/c/galay-kernel-c/async-c/file_coro_c.h`
- Create: `src/c/galay-kernel-c/async-c/file_coro_c.cc`
- Test: `test/c/kernel/t27_coro_fileio_aio.c`
- Example: `examples/c/kernel/e14_coro_file_copy.c`
- Benchmark: `benchmark/c/kernel/b24_coro_fileio_throughput.c`
- Benchmark: `benchmark/c/kernel/b25_coro_aio_batch.c`
- Benchmark: `benchmark/c/kernel/b26_coro_fileio_vs_callback.c`

**APIs:**
- [ ] `C_IOResult galay_coro_async_file_read(file, buffer, length, offset, timeout_ms)`
- [ ] `C_IOResult galay_coro_async_file_write(file, buffer, length, offset, timeout_ms)`
- [ ] `C_IOResult galay_coro_aio_file_submit_batch(file, requests, count, timeout_ms)`

**Tests and metrics:**
- [ ] Read/write correctness.
- [ ] Offset boundaries.
- [ ] Truncated/closed file behavior.
- [ ] Timeout behavior.
- [ ] Throughput MB/s, ops/sec, p50/p90/p99 latency.
- [ ] Compare against existing callback file/AIO path.

**Completion:**
- [ ] Completed and verified.

---

## Task 9: Timeout API And Timer Integration

**Files:**
- Create: `src/c/galay-kernel-c/coro-c/coro_timeout_c.h`
- Create: `src/c/galay-kernel-c/coro-c/coro_timeout_c.cc`
- Test: `test/c/kernel/t28_coro_timeout.c`
- Example: `examples/c/kernel/e15_coro_timeout.c`
- Benchmark: `benchmark/c/kernel/b27_coro_timeout_pressure.c`
- Benchmark: `benchmark/c/kernel/b28_coro_timeout_vs_callback.c`

**APIs:**
- [ ] `C_IOResult galay_coro_sleep(timeout_ms)`
- [ ] Timeout wrapper works for TCP/UDP/file APIs.

**Tests and metrics:**
- [ ] Sleep returns after requested duration.
- [ ] Zero timeout.
- [ ] Cancel before timeout.
- [ ] Timeout race with I/O completion.
- [ ] p50/p90/p99 wake latency.
- [ ] QPS under timeout pressure.
- [ ] Compare against old callback timeout pressure benchmark.

**Completion:**
- [ ] Completed and verified.

---

## Task 10: Examples Matrix

**Files:**
- Modify: `examples/c/kernel/CMakeLists.txt`
- Create examples from Tasks 6-9 if not already created.

**Examples required:**
- [ ] TCP coroutine echo.
- [ ] UDP coroutine echo.
- [ ] AIO batch coroutine example.
- [ ] Async file copy coroutine example.
- [ ] Timeout/sleep coroutine example.

**Verification:**
- [ ] `rtk cmake --build build-coro --target examples_c_kernel`
- [ ] Each example has a short source comment describing expected command and behavior.

**Completion:**
- [ ] Completed and verified.

---

## Task 11: Benchmark Matrix And Comparison Report

**Files:**
- Modify: `benchmark/c/kernel/CMakeLists.txt`
- Create or update benchmark sources from Tasks 6-9.
- Create: `docs/c/modules/kernel/06-C协程性能对比.md`

**Benchmark requirements:**
- [ ] TCP coroutine vs callback/C++ Task bridge.
- [ ] UDP coroutine vs callback/C++ Task bridge.
- [ ] AIO coroutine vs callback/C++ Task bridge.
- [ ] File I/O coroutine vs callback/C++ Task bridge.
- [ ] Timeout coroutine vs callback/C++ Task bridge.

**Each result row must include:**
- [ ] backend: epoll, io_uring, or kqueue.
- [ ] io scheduler count.
- [ ] connections/clients/concurrency.
- [ ] payload or block size.
- [ ] duration.
- [ ] total operations.
- [ ] QPS.
- [ ] throughput MB/s where applicable.
- [ ] latency p50/p90/p99.
- [ ] errors/timeouts/cancellations.
- [ ] old callback path result.
- [ ] new C coroutine path result.
- [ ] delta percentage.

**Verification:**
- [ ] All benchmark targets build.
- [ ] Local benchmark run completed or exact environment blocker recorded.
- [ ] Report file updated with commands and raw output excerpts.

**Completion:**
- [ ] Completed and verified.

---

## Task 12: Final Regression And Review

**Commands:**
- [ ] `rtk cmake --build build-coro -j`
- [ ] `rtk ctest --test-dir build-coro --output-on-failure`
- [ ] `rtk cmake --build build-coro --target benchmark_c_kernel_coro_tcp_echo_throughput benchmark_c_kernel_coro_udp_throughput benchmark_c_kernel_coro_fileio_throughput benchmark_c_kernel_coro_timeout_pressure`
- [ ] `rtk rg -n "runtime->spawn\\(|Task<void> c_api_" src/c/galay-kernel-c/coro-c src/c/galay-kernel-c/async-c/*coro*`
- [ ] `rtk git status --short`

**Review gates:**
- [ ] Spec compliance review approved.
- [ ] Code quality review approved.
- [ ] C++ hot path performance regression assessed.
- [ ] C API docs/examples complete.
- [ ] This plan has every completed item checked with verification evidence.

**Completion:**
- [ ] Completed and verified.
