# Directory Convergence Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Move public headers into `process/`, `cache/`, `tool/`, and `algorithm/` so the repository has fewer, clearer module directories.

**Architecture:** This is a public include-path migration with no compatibility headers. Each batch moves one semantic group, updates all direct includes, umbrella/module facades, tests, docs, examples, and benchmark includes, then verifies the build before continuing.

**Tech Stack:** C++23 header-only utilities, CMake/CTest, direct includes, C++23 module facade, RTK command wrapper.

---

## Progress Ledger

- [ ] Task 1: Move process-boundary headers into `galay-utils/process/`
- [ ] Task 2: Move cache/buffer helpers into `galay-utils/cache/` and engineering tools into `galay-utils/tool/`
- [ ] Task 3: Move algorithms and data structures into `galay-utils/algorithm/`
- [ ] Task 4: Update docs, roadmap, and import smoke coverage
- [ ] Task 5: Full verification and stale-path audit

## Non-Goals

- Do not keep compatibility headers at old paths.
- Do not change runtime behavior while moving headers.
- Do not reintroduce `galay-kernel` dependencies.
- Do not migrate `TimingWheelTimerManager`, `ThreadSafeTimerManager`, `TimerScheduler`, or `TimeoutTimer`.
- Do not commit unless explicitly requested.

## Task 1: Move Process-Boundary Headers

**Files:**
- Move: `galay-utils/process/system.hpp` -> `galay-utils/process/system.hpp`
- Move: `galay-utils/process/process.hpp` -> `galay-utils/process/process.hpp`
- Move: `galay-utils/process/signal.hpp` -> `galay-utils/process/signal.hpp`
- Move: `galay-utils/process/backtrace.hpp` -> `galay-utils/process/backtrace.hpp`
- Modify: `galay-utils/galay_utils.hpp`
- Modify: `galay-utils/module/galay_utils.cppm`
- Modify: `test/platform/platform_test.cpp`
- Modify: `test/import_smoke.cpp`
- Modify: docs that reference legacy platform include globs

**Step 1: Move files**

Use `git mv`:

```bash
rtk git mv galay-utils/process/system.hpp galay-utils/process/system.hpp
rtk git mv galay-utils/process/process.hpp galay-utils/process/process.hpp
rtk git mv galay-utils/process/signal.hpp galay-utils/process/signal.hpp
rtk git mv galay-utils/process/backtrace.hpp galay-utils/process/backtrace.hpp
```

**Step 2: Update includes**

Replace current public include paths:

- `galay-utils/process/system.hpp` -> `galay-utils/process/system.hpp`
- `galay-utils/process/process.hpp` -> `galay-utils/process/process.hpp`
- `galay-utils/process/signal.hpp` -> `galay-utils/process/signal.hpp`
- `galay-utils/process/backtrace.hpp` -> `galay-utils/process/backtrace.hpp`

Update these files first:

- `galay-utils/galay_utils.hpp`
- `galay-utils/module/galay_utils.cppm`
- `test/platform/platform_test.cpp`
- `test/import_smoke.cpp`
- `README.md`
- `docs/02-API参考.md`
- `docs/03-使用指南.md`
- `docs/04-示例代码.md`
- `docs/06-高级主题.md`
- `docs/07-常见问题.md`
- `docs/plans/2026-06-06-module-production-roadmap.md`

**Step 3: Run narrow verification**

```bash
rtk cmake --build cmake-build-test --target platform_test
rtk ctest --test-dir cmake-build-test -R '^platform_test$' --output-on-failure
rtk proxy git diff --check
```

Expected: build succeeds, `platform_test` passes, no whitespace errors.

## Task 2: Move Cache/Buffer Helpers and Engineering Tools

**Files:**
- Move: `galay-utils/tool/lru_cache.hpp` -> `galay-utils/cache/lru_cache.hpp`
- Move: `galay-utils/concurrency/thread.hpp` -> `galay-utils/tool/thread.hpp`
- Move: `galay-utils/concurrency/pool.hpp` -> `galay-utils/tool/pool.hpp`
- Move: `galay-utils/tool/byte_queue_view.hpp` -> `galay-utils/cache/byte_queue_view.hpp`
- Move: `galay-utils/tool/ring_buffer.hpp` -> `galay-utils/cache/ring_buffer.hpp`
- Move: `galay-utils/tool/rate_limiter.hpp` -> `galay-utils/tool/rate_limiter.hpp`
- Move: `galay-utils/tool/circuit_breaker.hpp` -> `galay-utils/tool/circuit_breaker.hpp`
- Move: `galay-utils/tool/balancer.hpp` -> `galay-utils/tool/balancer.hpp`
- Move: `galay-utils/tool/balancer.inl` -> `galay-utils/tool/balancer.inl`
- Modify: umbrella/module facades, tests, benchmarks, docs

**Step 1: Move files**

```bash
rtk git mv galay-utils/tool/lru_cache.hpp galay-utils/cache/lru_cache.hpp
rtk git mv galay-utils/concurrency/thread.hpp galay-utils/tool/thread.hpp
rtk git mv galay-utils/concurrency/pool.hpp galay-utils/tool/pool.hpp
rtk git mv galay-utils/tool/byte_queue_view.hpp galay-utils/cache/byte_queue_view.hpp
rtk git mv galay-utils/tool/ring_buffer.hpp galay-utils/cache/ring_buffer.hpp
rtk git mv galay-utils/tool/rate_limiter.hpp galay-utils/tool/rate_limiter.hpp
rtk git mv galay-utils/tool/circuit_breaker.hpp galay-utils/tool/circuit_breaker.hpp
rtk git mv galay-utils/tool/balancer.hpp galay-utils/tool/balancer.hpp
rtk git mv galay-utils/tool/balancer.inl galay-utils/tool/balancer.inl
```

**Step 2: Update includes**

Replace paths:

- `galay-utils/tool/lru_cache.hpp` -> `galay-utils/cache/lru_cache.hpp`
- `galay-utils/concurrency/thread.hpp` -> `galay-utils/tool/thread.hpp`
- `galay-utils/concurrency/pool.hpp` -> `galay-utils/tool/pool.hpp`
- `galay-utils/tool/byte_queue_view.hpp` -> `galay-utils/cache/byte_queue_view.hpp`
- `galay-utils/tool/ring_buffer.hpp` -> `galay-utils/cache/ring_buffer.hpp`
- `galay-utils/tool/rate_limiter.hpp` -> `galay-utils/tool/rate_limiter.hpp`
- `galay-utils/tool/circuit_breaker.hpp` -> `galay-utils/tool/circuit_breaker.hpp`
- `galay-utils/tool/balancer.hpp` -> `galay-utils/tool/balancer.hpp`
- `galay-utils/tool/balancer.inl` -> `galay-utils/tool/balancer.inl`

Update all in-repo references in:

- `galay-utils/`
- `test/`
- `benchmark/`
- `examples/`
- `README.md`
- `docs/*.md`
- `docs/plans/*.md`

**Step 3: Run grouped verification**

```bash
rtk cmake --build cmake-build-test --target cache_test buffer_test resilience_test routing_test
rtk ctest --test-dir cmake-build-test -R 'cache_test|buffer_test|resilience_test|routing_test' --output-on-failure
rtk cmake --build cmake-build-test --target concurrency_test
rtk ctest --test-dir cmake-build-test -R '^concurrency_test$' --output-on-failure
rtk proxy git diff --check
```

Expected: all five groups pass.

## Task 3: Move Algorithms and Data Structures

**Files:**
- Move: `galay-utils/algorithm/consistent_hash.hpp` -> `galay-utils/algorithm/consistent_hash.hpp`
- Move: `galay-utils/algorithm/trie.hpp` -> `galay-utils/algorithm/trie.hpp`
- Move: `galay-utils/algorithm/huffman.hpp` -> `galay-utils/algorithm/huffman.hpp`
- Move: `galay-utils/algorithm/mvcc.hpp` -> `galay-utils/algorithm/mvcc.hpp`
- Modify: umbrella/module facades, tests, docs

**Step 1: Move files**

```bash
rtk git mv galay-utils/algorithm/consistent_hash.hpp galay-utils/algorithm/consistent_hash.hpp
rtk git mv galay-utils/algorithm/trie.hpp galay-utils/algorithm/trie.hpp
rtk git mv galay-utils/algorithm/huffman.hpp galay-utils/algorithm/huffman.hpp
rtk git mv galay-utils/algorithm/mvcc.hpp galay-utils/algorithm/mvcc.hpp
```

**Step 2: Update includes**

Replace paths:

- `galay-utils/algorithm/consistent_hash.hpp` -> `galay-utils/algorithm/consistent_hash.hpp`
- `galay-utils/algorithm/trie.hpp` -> `galay-utils/algorithm/trie.hpp`
- `galay-utils/algorithm/huffman.hpp` -> `galay-utils/algorithm/huffman.hpp`
- `galay-utils/algorithm/mvcc.hpp` -> `galay-utils/algorithm/mvcc.hpp`

Update all in-repo references in:

- `galay-utils/`
- `test/`
- `benchmark/`
- `examples/`
- `README.md`
- `docs/*.md`
- `docs/plans/*.md`

**Step 3: Run grouped verification**

```bash
rtk cmake --build cmake-build-test --target data_test routing_test algorithm_test
rtk ctest --test-dir cmake-build-test -R 'data_test|routing_test|algorithm_test' --output-on-failure
rtk proxy git diff --check
```

Expected: all three groups pass.

## Task 4: Update Docs, Roadmap, and Import Smoke Coverage

**Files:**
- Modify: `README.md`
- Modify: `docs/02-API参考.md`
- Modify: `docs/03-使用指南.md`
- Modify: `docs/04-示例代码.md`
- Modify: `docs/06-高级主题.md`
- Modify: `docs/07-常见问题.md`
- Modify: `docs/plans/2026-06-06-module-production-roadmap.md`
- Modify: `test/import_smoke.cpp`

**Step 1: Update public header inventory**

Ensure `docs/02-API参考.md` lists these canonical paths:

- `galay-utils/process/*.hpp`
- `galay-utils/cache/*.hpp`
- `galay-utils/tool/*.hpp`
- `galay-utils/algorithm/*.hpp`

Remove active API references to:

- legacy platform public headers
- legacy cache public headers
- legacy buffer public headers
- legacy resilience public headers
- legacy routing public headers
- legacy data public headers

**Step 2: Update usage wording**

Use these group names:

- 进程与系统边界：`process/`
- 缓存与缓冲：`cache/`
- 工程工具与阻塞资源工具：`tool/`
- 算法与数据结构：`algorithm/`

Do not keep `concurrency/` as a public include directory. Document `ThreadPool`, `TaskWaiter`, `ObjectPool`, and `BlockingObjectPool` under `tool/`, with their blocking/thread ownership semantics called out explicitly.

**Step 3: Update import smoke**

`test/import_smoke.cpp` should consume at least:

- `System` or `Process` from process group
- `LruCache`, `ByteQueueView`, `RingBuffer` from cache group
- `CircuitBreaker`, `RoundRobinLoadBalancer` from tool group
- `ConsistentHash`, `TrieTree`, `Mvcc`, one Huffman type if already used from algorithm group

**Step 4: Run docs/source scan**

```bash
rtk proxy rg -n 'galay-utils/(platform|buffer|resilience|routing|data)/' galay-utils test examples benchmark README.md docs
```

Expected: no hits in source/tests/examples/benchmark/current API docs. Historical release notes or explicit migration discussion may remain only if clearly historical.

## Task 5: Full Verification and Stale-Path Audit

**Files:**
- All moved headers and all modified docs/tests.

**Step 1: Build full test tree**

```bash
rtk cmake --build cmake-build-test
```

Expected: build succeeds.

**Step 2: Run full CTest**

```bash
rtk ctest --test-dir cmake-build-test --output-on-failure
```

Expected: all tests pass.

**Step 3: Run diff check**

```bash
rtk proxy git diff --check
```

Expected: no whitespace errors.

**Step 4: Verify installed inline header coverage**

```bash
rtk cmake --install cmake-build-test --prefix /tmp/galay-utils-install-check
rtk proxy test -f /tmp/galay-utils-install-check/include/galay-utils/tool/balancer.inl && echo exists
```

Expected: prints `exists`.

**Step 5: Review status**

```bash
rtk git status --short
rtk git diff --stat
```

Expected: only intended directory convergence changes are present.
