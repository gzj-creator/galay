# ThreadPool ConcurrentQueue Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace the mutex-protected `ThreadPool` task queue with moodycamel `ConcurrentQueue` / `BlockingConcurrentQueue` integration.

**Architecture:** `galay-utils` will model `concurrentqueue` the same way `galay-kernel` does: locate headers under `<concurrentqueue/moodycamel/*.h>`, expose an imported `concurrentqueue::concurrentqueue` target, and link it through the header-only interface target. `ThreadPool` will use `moodycamel::BlockingConcurrentQueue<std::function<void()>>` for task delivery and atomics for stopped, pending, active, and completion counters. `TaskWaiter` will use atomic wait/notify instead of mutex/condition_variable.

**Tech Stack:** C++23 header-only utilities, moodycamel concurrentqueue headers, CMake/CTest.

---

### Task 1: Add Failing Tests

**Files:**
- Modify: `test/concurrency/concurrency_test.cpp`
- Modify: `test/CMakeLists.txt`

**Steps:**
1. Add source inspection assertions that `galay-utils/tool/thread.hpp` no longer contains `std::mutex`, `std::condition_variable`, `lock_guard`, or `unique_lock` in the thread pool/waiter implementation.
2. Add behavioral coverage for `pendingTasks()`, `waitAll()`, `stopNow()`, and add-after-stop.
3. Run `rtk cmake --build cmake-build-test --target concurrency_test`.
4. Expected before implementation: compile or assertion failure because the header still uses mutex/cv.

### Task 2: Add ConcurrentQueue Dependency Plumbing

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `cmake/galay-utils-config.cmake.in`

**Steps:**
1. Add `GALAY_UTILS_CONCURRENTQUEUE_INCLUDE_DIR` cache variable.
2. Find `concurrentqueue/moodycamel/concurrentqueue.h` and `blockingconcurrentqueue.h` under common prefixes.
3. Create imported `concurrentqueue::concurrentqueue` target.
4. Link `galay-utils` to that interface target.

### Task 3: Refactor ThreadPool

**Files:**
- Modify: `galay-utils/tool/thread.hpp`

**Steps:**
1. Include `<concurrentqueue/moodycamel/blockingconcurrentqueue.h>`.
2. Replace `std::queue`, `std::mutex`, and `std::condition_variable` with `moodycamel::BlockingConcurrentQueue<std::function<void()>>`.
3. Use atomic counters and `atomic::wait/notify_all` for `waitAll()` and `TaskWaiter`.
4. Preserve public API behavior: `addTask` throws after stop, `execute` returns silently after stop, `stop()` drains queued tasks, `stopNow()` drops queued tasks.

### Task 4: Update Docs

**Files:**
- Modify: `README.md`
- Modify: `docs/00-快速开始.md`
- Modify: `docs/02-API参考.md`
- Modify: `docs/06-高级主题.md`
- Modify: `docs/07-常见问题.md`

**Steps:**
1. Document the new `concurrentqueue` header dependency.
2. Clarify that submit path no longer uses mutex/cv, while waits are still blocking thread APIs.

### Task 5: Verify

Run:

```bash
rtk cmake --build cmake-build-test --target concurrency_test
rtk ctest --test-dir cmake-build-test -R '^concurrency_test$' --output-on-failure
rtk cmake --build cmake-build-test
rtk ctest --test-dir cmake-build-test --output-on-failure
rtk proxy git diff --check
```
