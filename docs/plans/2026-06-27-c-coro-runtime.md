# C/C++ Coroutine Runtime Unification Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a first-class high-performance C stackful coroutine runtime that shares the existing galay scheduler, timer, and epoll/io_uring/kqueue backends with C++23 `Task<T>`, without routing every C operation through a freshly spawned C++ `Task<void>`.

**Architecture:** Keep C++ and C coroutine objects separate, but unify their scheduling and I/O wakeup boundary through a low-overhead `ReadyEntry` / `ResumeToken` model. C++ keeps `co_await` and `std::coroutine_handle<>`; C gets `galay_coro_*` stackful APIs returning `C_IOResult` structs. The existing C callback ABI remains compatible and serves as the baseline comparison path.

**Tech Stack:** C++23, C11 ABI wrappers, platform context switch assembly (Darwin arm64 in Task 4; Linux/x86_64 portability is a follow-up before final delivery), CMake, CTest, existing galay kernel runtime, epoll/io_uring/kqueue reactors, benchmark targets under `benchmark/c/kernel` and `benchmark/cpp/kernel`.

---

## Non-Negotiable Requirements

- [x] Work only inside worktree: `/Users/gongzhijie/Desktop/projects/git/galay/.wroktree/c-coro-runtime`.
- [ ] Do not regress existing C++ `Task<T>` / `co_await` public APIs.
- [ ] Do not make C coroutine APIs spawn a C++ `Task<void>` per I/O operation.
- [ ] Keep old C callback APIs working.
- [ ] C coroutine user APIs return result structs, not callback-only results.
- [ ] Every production change is preceded by a failing test.
- [ ] Every completed task updates this document's checkbox and records exact verification commands.
- [ ] Final examples cover TCP, UDP, AIO, file I/O, and timeout APIs.
- [ ] Final benchmarks report throughput, QPS, latency p50/p90/p99, error count, and compare against the current C callback path that internally spawns C++ coroutines.

## Core Design Decisions

- [x] Scheduler ready queues store a language-neutral ready item, not only `TaskRef`.
- [x] C++ ready item resumes `TaskState::m_handle.resume()`.
- [x] C ready item resumes a `C_CoroTask` using C context switching.
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
- [x] New `galay_coro_*` implementation files do not contain `runtime->spawn(`.
- [x] New `galay_coro_*` implementation files do not contain `Task<void> c_api_`.
- [x] Legacy callback files may still contain the old bridge; boundary test is targeted.

**Verification:**
- [x] RED/registration baseline: `rtk ctest --test-dir build-coro -R t22_coro_source_boundaries --output-on-failure` reported `No tests were found!!!` before CMake registration. Current tree has no `src/c/galay-kernel-c/coro-c` or other C `*coro*` implementation files, so the meaningful source-boundary RED form is represented by the new test's legacy detector self-test: it must find existing `runtime->spawn(` and `Task<void> c_api_` bridge tokens in the old callback TCP C API before the future-coro negative scan can pass.
- [x] Register/build: `rtk cmake -S . -B build-coro -DBUILD_TESTING=ON -DGALAY_BUILD_C_API=ON -DGALAY_BUILD_BENCHMARKS=ON -DGALAY_BUILD_EXAMPLES=ON` passed; `rtk cmake --build build-coro --target test_c_kernel_t22_coro_source_boundaries` passed.
- [x] GREEN: `rtk ctest --test-dir build-coro -R t22_coro_source_boundaries --output-on-failure` passed.
- [x] Follow-up RED: `rtk ctest --test-dir build-coro -R t22_coro_source_boundaries --output-on-failure -V` previously returned 0 while printing `PASS; checked 0 future C coroutine source file(s)`, which was a false PASS.
- [x] Follow-up GREEN: `rtk cmake --build build-coro --target test_c_kernel_t22_coro_source_boundaries && rtk ctest --test-dir build-coro -R t22_coro_source_boundaries --output-on-failure -V` passed and now prints `T22-CoroSourceBoundaries SKIP; no future C coroutine source files found under src/c/galay-kernel-c, so bridge boundary checks are not active yet`.

**Completion:**
- [x] Completed and verified.

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
- [x] Add internal `ReadyEntry` equivalent with packed kind/state.
- [x] Make `ReadyEntry` move-only RAII so queued references are released on destruction unless ownership is transferred.
- [x] Keep a C++ fast path conversion from `TaskRef` to `ReadyEntry`.
- [x] Define C coroutine ready-entry hooks for owner scheduler lookup, owner-only resume constraints, resume, and release.
- [x] Keep `TaskRef` public behavior unchanged.
- [x] Owner-thread resume still clears `m_queued` and `m_resume_owner_only` for C++ tasks.
- [x] Avoid `std::function`, heap allocation, and virtual `Coroutine::resume()` on the hot path.

**Tests:**
- [x] C++ `Task<void>` spawn still completes.
- [x] `Task<T>` result propagation still works.
- [x] `co_await Task<T>` still resumes parent.
- [x] `then()` still schedules continuation.
- [x] Cross-thread schedule still wakes and resumes on the owner scheduler thread.
- [x] Unsupported C coroutine schedule path rejects without releasing or invalidating the entry.
- [x] Unsupported C++ `scheduleReadyEntry()` rejection preserves the caller-owned entry for fallback/release.
- [x] Owner-only C coroutine `ReadyEntry` is not stolen; once the ring owns the slot, it returns the entry to the caller instead of peeking pre-CAS.
- [x] Worker-level owner-only entries are requeued through the victim inject queue, not pushed back into the victim ring from a stealer thread.
- [x] Pending `ReadyEntry` cleanup releases LIFO, ring, inject queue, and inject buffer entries.

**Verification:**
- [x] RED: `rtk cmake --build build-coro --target t135_ready_entry_cpp_compat` failed before production changes after extending `t135` for move-only `ReadyEntry`, C coroutine hook APIs, owner-only stealing, and pending-entry cleanup.
- [x] Implementation guardrail: `rtk cmake --build build-coro --target t135_ready_entry_cpp_compat` failed during the first RAII implementation on an attempted `ReadyEntry` copy assignment in `ChaseLevTaskRing::steal_front`, confirming move-only ownership was enforced by the compiler.
- [x] RED: `rtk ctest --test-dir build-coro -R ready_entry_cpp_compat --output-on-failure` failed with `worker should requeue post-CAS owner-only race through inject queue` before moving post-CAS requeue out of `ChaseLevTaskRing` and into `IOSchedulerWorkerState`.
- [x] Review RED: `rtk ctest --test-dir build-coro -R ready_entry_cpp_compat --output-on-failure` failed with `rejected C++ schedule should keep ReadyEntry owned by caller` before preserving C++ `scheduleReadyEntry()` false-return ownership.
- [x] Review fix: removed the pre-CAS owner-only peek from `ChaseLevTaskRing::steal_front()` so owner-only hooks are queried only after the stealer owns the slot; `IOSchedulerWorkerState::stealFront()` now requeues returned owner-only entries through the victim inject queue.
- [x] Build: `rtk cmake --build build-coro --target t135_ready_entry_cpp_compat test_c_kernel_t22_coro_source_boundaries benchmark_kernel_ready_entry_wakeup_latency` passed.
- [x] GREEN: `rtk ctest --test-dir build-coro -R ready_entry_cpp_compat --output-on-failure` passed 1/1.
- [x] Extended regression: `rtk ctest --test-dir build-coro -R "kernel\\.(ready_entry_cpp_compat|wakecoal|ready|qwake|then_runtime|followpass|ringfb|ringsteal|stealstats|localfifo|ringshutdown|spawn|chain)$|t22_coro_source_boundaries" --output-on-failure` passed 14/14.
- [x] Benchmark run: `rtk ./build-coro/benchmark/cpp/kernel/benchmark_kernel_ready_entry_wakeup_latency` passed on kqueue, 5000 samples, avg 8.61us, p50 5.67us, p90 12.62us, p99 46.83us, sampled_wakes_per_sec 398. The benchmark uses one reusable producer thread, so the rate is a sequential sampled wake rate, not sustained wake throughput.
- [x] Diff hygiene: `rtk git diff --check` passed.
- [x] Review: spec re-review PASS; code-quality re-review PASS with no Critical/Important/Minor findings.

**Completion:**
- [x] Completed and verified.

---

## Task 3: Generalize Waker To ResumeToken

**Purpose:** Let I/O completion schedule either C++ `TaskRef` or C `C_CoroTask` without knowing the language.

**Files:**
- Modify: `src/cpp/galay-kernel/core/task.h`
- Modify: `src/cpp/galay-kernel/core/task.cc`
- Modify: `src/cpp/galay-kernel/core/waker.h`
- Modify: `src/cpp/galay-kernel/core/waker.cc`
- Modify: `test/cpp/kernel/t31_wakepath.cc`
- Test: `test/cpp/kernel/t136_resume_token_waker.cc`
- Benchmark: `benchmark/cpp/kernel/b19_resume_token_waker_ops.cc`

**Design:**
- [x] `Waker(TaskRef)` remains source-compatible.
- [x] Add internal copyable `detail::ResumeToken` and `Waker(detail::ResumeToken)` constructor.
- [x] Keep `Waker` pointer-sized with low-bit tagged state encoding.
- [x] `Waker::wakeUp()` requests resume through the token owner; C++ uses `requestTaskResumeState(TaskState*)`, C hook uses `request_resume`.
- [x] Existing C++ awaitables still obtain scheduler from suspended C++ promise.
- [x] No public C++ coroutine API changes.

**Tests:**
- [x] Fake C resume token hook exposes owner scheduler, requests resume, and retain/releases on Waker copy/destruction.
- [x] Misaligned C resume token state is rejected before hook access and does not retain/release.
- [x] C++ Waker still wakes through generalized token and resumes on owner scheduler thread.
- [x] Duplicate wake still coalesces for C++ tasks.
- [x] Invalid Waker is ignored safely.
- [x] Existing wakepath static checks verify `sizeof(Waker) == sizeof(void*)` and ReadyEntry-backed worker fields.

**Verification:**
- [x] RED: `rtk cmake --build build-coro --target t136_resume_token_waker` failed before implementation with missing `detail::ResumeTokenHeader`, `detail::ResumeTokenHooks`, `detail::ResumeToken`, and `Waker(detail::ResumeToken)`.
- [x] Guardrail RED: `rtk cmake --build build-coro --target t31_wakepath ...` failed while `Waker` was 24 bytes; implementation was changed to tagged pointer storage so `sizeof(Waker) == sizeof(void*)` remains true.
- [x] Build: `rtk cmake --build build-coro --target t31_wakepath t136_resume_token_waker benchmark_kernel_resume_token_waker_ops` passed.
- [x] GREEN: `rtk ctest --test-dir build-coro -R "kernel\\.(wakepath|resume_token_waker)$" --output-on-failure` passed 2/2.
- [x] Extended Waker regression: `rtk ctest --test-dir build-coro -R "kernel\\.(wakepath|resume_token_waker|refpath|contpath|wakecoal|qwake|ready|ready_entry_cpp_compat|then_runtime|followpass|spawn|chain)$" --output-on-failure` passed 12/12.
- [x] Benchmark run: `rtk ./build-coro/benchmark/cpp/kernel/benchmark_kernel_resume_token_waker_ops` passed, wake_only_qps 119832235, copy_wake_qps 25341289, wake_requests 1000000, wake_retains 1, copy_requests 1000000, copy_retains 1000001, copy_releases 1000001.
- [x] Review fix: code-quality review found the initial C hook token did not retain before release; `ResumeToken::fromCCoroutine()` now acquires one hook reference on creation, and T136/B19 verify symmetric retain/release counts.
- [x] Review fix: code-quality review found an encode-failure path after retain; `ResumeToken::fromCCoroutine()` now validates tagged-pointer encoding before hook access/retain, and T136 covers misaligned token rejection.

**Completion:**
- [x] Completed and verified.

---

## Task 4: Add C Coroutine Core

**Purpose:** Add first-class C stackful coroutine task objects and scheduler integration without I/O yet.

**Files:**
- Create: `src/c/galay-kernel-c/coro-c/coro_result_c.h`
- Create: `src/c/galay-kernel-c/coro-c/coro_task_c.h`
- Create: `src/c/galay-kernel-c/coro-c/coro_task_internal.hpp`
- Create: `src/c/galay-kernel-c/coro-c/coro_task_c.cc`
- Create: `src/c/galay-kernel-c/coro-c/coro_context_aarch64.S`
- Modify: `src/c/galay-kernel-c/CMakeLists.txt`
- Test: `test/c/kernel/t23_coro_task.c`
- Test: `test/cpp/kernel/t137_c_coro_resume_token.cc`
- Benchmark: `benchmark/c/kernel/b19_coro_yield_requeue_latency.c`

**C API shape:**
- [x] `galay_coro_spawn(runtime, entry_fn, arg, options, out_task)`
- [x] `galay_coro_yield()`
- [x] `galay_coro_current()`
- [x] `galay_coro_cancel(task)`
- [x] `galay_coro_join(task, timeout_ms)`
- [x] `galay_coro_destroy(task)`

**Runtime rules:**
- [x] Fixed owner C coroutine runtime state for each C coroutine.
- [x] No cross-thread direct resume in the core scheduler.
- [x] Stack allocated from explicit `mmap` stack allocator.
- [x] Guard page via `mprotect` on supported platform.
- [x] `C_CoroTask` status: ready, running, done, cancelled. Wait-suspended state is deferred to Task 5.
- [x] State transitions use CAS for `Ready -> Running` and `Ready -> Cancelled`.
- [x] `galay_coro_current()` returns an owning handle; borrowed-current UAF is not exposed.
- [x] C coroutine task exposes a real `ResumeToken` hook; `request_resume` reuses `request_ready()`.
- [x] `join()` rejects calls from C coroutine / owner scheduler thread until Task 5 cooperative wait exists.
- [x] `join()` rejects calls from any galay scheduler thread through `SchedulerThreadScope`, preventing blocking waits from C++ scheduler coroutines too.
- [x] Context-switch tests and benchmarks are gated by `GALAY_C_CORO_CONTEXT_SUPPORTED` so non-Darwin-arm64 platforms can still build the C API library before portable assembly lands.
- [x] Recoverable failures return typed `C_IOResult` structs.
- [x] No `abort`, `exit`, or hidden process termination.

**Tests:**
- [x] Spawn/yield/resume completes.
- [x] Multiple coroutines can yield/resume and complete; exact fairness order is not part of the Task 4 API.
- [x] Cancel ready coroutine before join returns `C_IOResultCancelled`.
- [x] Destroy after completion/cancel clears handles.
- [x] Invalid parameters return errors.
- [x] `galay_coro_current()` returns the current C coroutine from inside the coroutine.
- [x] `galay_coro_join()` from inside a C coroutine returns `C_IOResultInvalid`.
- [x] Destroying a running/not-yet-terminal task returns `C_IOResultInvalid`.
- [x] Done task cancellation preserves the original result.
- [x] C++ exceptions escaping a C entry are caught and reported as `C_IOResultError`.
- [x] Real C coroutine task can construct a `ResumeToken` / `Waker` with the owner scheduler.

**Verification:**
- [x] RED: `rtk cmake --build build-coro --target test_c_kernel_coro_task` failed before implementation with missing `galay/c/galay-kernel-c/coro-c/coro_task_c.h`.
- [x] GREEN: `rtk cmake --build build-coro --target test_c_kernel_coro_task && rtk ctest --test-dir build-coro -R coro_task --output-on-failure` passed.
- [x] Review RED: `rtk cmake --build build-coro --target test_c_kernel_coro_task t137_c_coro_resume_token` failed before fixes because `t137_c_coro_resume_token` could not include `src/c/galay-kernel-c/coro-c/coro_task_internal.hpp`; `rtk ctest --test-dir build-coro -R coro_task --output-on-failure` failed after adding owning-current/state-machine assertions.
- [x] Review GREEN: `rtk ctest --test-dir build-coro -R "c\\.kernel\\.(header_smoke|runtime_lifecycle|coro_task|t22_coro_source_boundaries)|kernel\\.(ready_entry_cpp_compat|resume_token_waker|c_coro_resume_token|wakepath)$" --output-on-failure` passed 8/8.
- [x] Review fix: IO/compute scheduler `start()` now waits until `m_threadId` is published before returning, and C task `owner_thread` is initialized once at spawn instead of being lazily written from `ready_resume()`.
- [x] Review fix: scheduler worker threads now set `SchedulerThreadScope`; `t137_c_coro_resume_token` verifies `galay_coro_join()` is rejected from a compute scheduler coroutine.
- [x] Review fix: `t23_coro_task`, `t137_c_coro_resume_token`, and `b19_coro_yield_requeue_latency` are only registered when `GALAY_C_CORO_CONTEXT_SUPPORTED` is true.
- [x] Benchmark builds: `rtk cmake --build build-coro --target benchmark_c_kernel_coro_yield_requeue_latency` passed.
- [x] Benchmark run: `rtk ./build-coro/benchmark/c/kernel/benchmark_c_kernel_coro_yield_requeue_latency` passed, `CoroSchedulerYieldLatency mode=owner_yield_requeue, samples=20000, qps=3955696, avg=0.22us, p50=0.00us, p90=1.00us, p99=1.00us, errors=0`.
- [x] Diff hygiene: `rtk git diff --check` passed.
- [x] Review: final spec review PASS; final code-quality review PASS with no Critical/Important/Minor findings.

**Completion:**
- [x] Completed and verified for Darwin arm64 stackful core after review fixes. x86_64/Linux assembly and wait-suspended state remain later portability/work-wait follow-ups.

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
