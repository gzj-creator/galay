# Module Production Hardening Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Bring the remaining galay-utils modules to production quality while consolidating scattered files, tests, examples, and documentation.

**Architecture:** First stabilize the project layout and test entrypoints, then harden modules by risk group. Keep public API compatibility when moving headers by updating canonical includes, install rules, docs, and module facade together; do not keep compatibility headers unless the user explicitly asks for them.

**Tech Stack:** C++23 header-only utilities, CMake/CTest, optional benchmarks, direct includes, C++23 module facade.

---

## Progress Ledger

Complete exactly one checkbox when that plan is fully implemented and verified.

- [x] Plan 1: Directory and test layout consolidation
- [x] Plan 2: Core pure utility hardening
- [ ] Plan 3: System and platform utility hardening
- [ ] Plan 4: Concurrency and resource utility hardening
- [ ] Plan 5: Flow control, routing, and resilience hardening
- [ ] Plan 6: Data structure, parser, and CLI hardening
- [ ] Plan 7: Documentation, examples, and release readiness

## Non-Goals

- Do not reintroduce galay-kernel dependencies into galay-utils.
- Do not migrate `TimingWheelTimerManager`, `ThreadSafeTimerManager`, `TimerScheduler`, or `TimeoutTimer` into this repository.
- Do not keep compatibility headers after header moves unless the user explicitly asks for a compatibility window.
- Do not add mutexes to coroutine-facing non-thread-safe utilities just to hide synchronization from callers.
- Do not turn benchmarks into default build targets.

## Execution Rules

- Use TDD for behavior changes: write or move the failing test first, verify red when applicable, implement, verify green.
- Preserve a clear canonical public include path for every supported API.
- When moving or renaming public headers, update all of these together:
  - `galay-utils/galay_utils.hpp`
  - `galay-utils/module/galay_utils.cppm`
  - `docs/02-API参考.md`
  - `docs/03-使用指南.md`
  - `docs/04-示例代码.md`
  - `README.md`
  - `test/import_smoke.cpp`
- Run these before marking any plan complete:
  - `rtk cmake --build cmake-build-test`
  - `rtk ctest --test-dir cmake-build-test --output-on-failure`
  - `rtk proxy git diff --check`
- Run `rtk cmake --build cmake-build-bench` only for plans that touch benchmarked hot paths or benchmark CMake files.
- Do not commit unless the user explicitly requests a commit.

---

## Target Layout

The current layout is usable but scattered. Consolidate by responsibility:

| Current Area | Target Area | Notes |
|---|---|---|
| `string/`, `random/`, `time/`, `common/` | `core/` or `common/` | Prefer `core/` for public pure helpers and `common/` for low-level definitions. |
| `system/`, `process/`, `signal/`, `backtrace/` | `process/` | Process and OS-boundary helpers share ownership and failure-mode documentation. |
| `cache/`, `buffer/` | `cache/` | Cache and buffer utilities share one data-residency include path. |
| `thread/`, `pool/`, `ratelimiter/`, `circuitbreaker/`, `balancer/` | `tool/` | Business-facing engineering and blocking resource tools are grouped under one include path; blocking semantics stay documented at API level. |
| `consistent_hash/`, `trie/`, `mvcc/`, `huffman/` | `algorithm/` | Algorithms and data structures share the canonical algorithm include path. |
| `args/`, `parser/` | `app/` and `config/` | CLI and config parsing are distinct application support areas. |
| `algorithm/` | `crypto/` and `encoding/` | Split hashes/HMAC/salt from Base64. |
| `test/<area>/*_test.cpp` | `test/<area>/*_test.cpp` | One CTest target per module or module group. |
| `examples/include/e1_basic.cpp` | `examples/include/<area>/*.cpp` | Match include examples with module/import examples where practical. |

Do header moves in small batches. Because compatibility headers are not desired, each move must update every in-repo include in the same commit.

---

## Plan 1: Directory and Test Layout Consolidation

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `test/CMakeLists.txt`
- Modify: `galay-utils/galay_utils.hpp`
- Modify: `galay-utils/module/galay_utils.cppm`
- Modify: `test/import_smoke.cpp`
- Move: `test/<area>/*_test.cpp` into grouped test files under `test/core/`, `test/platform/`, `test/concurrency/`, `test/resilience/`, `test/routing/`, `test/data/`, `test/app/`, `test/algorithm/`, `test/cache/`, `test/buffer/`
- Optional Move: public headers into target layout after tests are split
- Modify Docs: `docs/02-API参考.md`, `docs/03-使用指南.md`, `docs/04-示例代码.md`, `README.md`

**Scope:**

Create a stable test and include structure before changing module internals. This plan may move files, but it should avoid behavior changes except those required to keep builds green after moves.

**Implementation Checklist:**

- Split `test/<area>/*_test.cpp` by module group without changing assertions.
- Register each test executable with `add_test(...)`.
- Keep a lightweight aggregate test target if useful, but CTest must expose individual module groups.
- Extend `test/import_smoke.cpp` to import or include at least one API from each public group.
- Decide the first header move batch and update canonical include paths everywhere in the same patch.
- Remove stale include paths instead of adding compatibility headers.
- Update docs to describe the new layout and canonical include paths.

**Acceptance:**

- `rtk ctest --test-dir cmake-build-test -N` lists grouped tests instead of one monolithic `AllTests` only.
- Full CTest passes.
- No source file includes a removed public header path.
- Install rules still include all public `.hpp` files.

---

## Plan 2: Core Pure Utility Hardening

**Files:**
- Modify or Move: `galay-utils/core/string.hpp`
- Modify or Move: `galay-utils/core/random.hpp`
- Modify or Move: `galay-utils/core/time.hpp`
- Modify or Move: `galay-utils/core/type_name.hpp`
- Modify or Move: `galay-utils/common/defn.hpp`
- Test: `test/core/*_test.cpp`
- Optional Benchmark: `benchmark/core_*_benchmark.cpp`

**Scope:**

Harden pure utility modules and make their thread-safety and failure behavior explicit. `Randomizer` is the only core utility with internal locking and should be documented as blocking/thread-safe instead of coroutine-neutral.

**Implementation Checklist:**

- Add tests for `StringUtils` empty delimiters, repeated delimiters, quote mismatch, invalid hex, parse overflow, and float/integer edge cases.
- Add tests for `Time::formatTime` invalid format/null format and stable UTC formatting.
- Add tests for `TypeName` null demangle input and template/object overload consistency.
- Review `Randomizer` for deterministic seeding, range validation, empty charset, zero-length output, UUID format, and thread-safety docs.
- Decide whether `Randomizer` should expose a non-shared local generator type to avoid global mutex costs in coroutine hot paths.
- Add benchmarks only for string split/join/hex or random hot paths if performance claims are made.

**Acceptance:**

- Core tests pass as a standalone CTest group.
- API docs state which utilities are pure, which are internally locked, and which are not suitable for coroutine hot paths.
- No core utility depends on platform/process/signal headers.

---

## Plan 3: System and Platform Utility Hardening

**Files:**
- Modify or Move: `galay-utils/process/system.hpp`
- Modify or Move: `galay-utils/process/process.hpp`
- Modify or Move: `galay-utils/process/signal.hpp`
- Modify or Move: `galay-utils/process/backtrace.hpp`
- Test: `test/platform/*_test.cpp`
- Docs: `docs/06-高级主题.md`, `docs/07-常见问题.md`

**Scope:**

Make platform-facing behavior precise: OS differences, failure returns, resource ownership, signal handler risks, and shell execution safety.

**Implementation Checklist:**

- Add platform tests for file read/write/mmap empty file, missing file, directory listing, env var unset/empty, host resolution failure, and executable path.
- Add process tests that avoid destructive behavior: current pid, parent pid, simple command execution, output capture, invalid executable, wait failure.
- Document that `Process::execute()` and `executeWithOutput()` run through a shell and must not receive untrusted input.
- Audit resource cleanup around Windows handles, POSIX fds, `mmap`, `popen`, and process wait paths.
- Review `SignalHandler` callback path for async-signal-safety. If callbacks remain `std::function`, document it as process-level convenience, not a low-level signal-safe primitive.
- Keep crash handler tests limited to non-crashing smoke tests.

**Acceptance:**

- Platform tests avoid machine-specific assumptions and pass on the local macOS/Linux development environment.
- Public docs clearly separate safe convenience APIs from low-level signal/process primitives.
- No time APIs remain in `System`.

---

## Plan 4: Concurrency and Resource Utility Hardening

**Files:**
- Modify or Move: `galay-utils/tool/thread.hpp`
- Modify or Move: `galay-utils/tool/pool.hpp`
- Test: `test/concurrency/*_test.cpp`
- Docs: `docs/06-高级主题.md`, `docs/07-常见问题.md`

**Scope:**

Clarify blocking behavior and shutdown semantics for thread-owning and mutex/condition-variable utilities.

**Implementation Checklist:**

- Add tests for `ThreadPool` zero-thread construction, task exception propagation, `stop()`, `stopNow()`, `waitAll()`, `pendingTasks()`, and add-after-stop behavior.
- Add tests for `TaskWaiter` wait success, timeout, and task exception not causing permanent wait.
- Add tests for `ObjectPool` creator/destroyer behavior, reset behavior, max size behavior, shrink/clear, and object reuse.
- Add tests for `BlockingObjectPool` timeout, release wakeup, and destructor behavior under no waiters.
- Replace sleep-based tests with bounded waits on observable state.
- Document which APIs block threads and should not run on coroutine scheduler threads.

**Acceptance:**

- Concurrency tests are deterministic and do not rely on long sleeps.
- Docs explicitly label blocking/thread-safe utilities.
- No hidden coroutine-facing mutex is added to previously non-thread-safe utilities.

---

## Plan 5: Flow Control, Routing, and Resilience Hardening

**Files:**
- Modify or Move: `galay-utils/tool/rate_limiter.hpp`
- Modify or Move: `galay-utils/tool/circuit_breaker.hpp`
- Modify or Move: `galay-utils/tool/balancer.hpp`
- Modify or Move: `galay-utils/tool/balancer.inl`
- Modify or Move: `galay-utils/algorithm/consistent_hash.hpp`
- Test: `test/resilience/*_test.cpp`, `test/routing/*_test.cpp`
- Benchmark: `benchmark/ratelimiter_benchmark.cpp`, `benchmark/balancer_benchmark.cpp`, `benchmark/consistent_hash_benchmark.cpp`

**Scope:**

Harden stateful decision modules where bugs affect traffic distribution, throttling, and failure behavior.

**Implementation Checklist:**

- Add deterministic clock injection where limiter and circuit breaker tests currently depend on real time.
- Add rate limiter tests for zero capacity, zero rate, over-acquire, refill/leak boundaries, and sliding window boundary times.
- Add circuit breaker tests for closed/open/half-open transitions, fallback return type, exception path, reset, force open, and threshold edge values.
- Add load balancer tests for empty nodes, zero weights, mismatched weights, append after construction, weighted distribution sanity, and thread-safety docs.
- Add consistent hash tests for empty ring, duplicate node ids, remove missing node, healthy-node fallback, virtual-node count, and custom hash function behavior.
- Add benchmarks only after deterministic correctness tests are green.

**Acceptance:**

- Time-dependent tests use injected clocks or bounded deterministic advancement.
- Docs state which routing/resilience types are thread-safe, partially thread-safe, or caller-synchronized.
- Benchmark docs include workload size and machine-dependent caveat.

---

## Plan 6: Data Structure, Parser, and CLI Hardening

**Files:**
- Modify or Move: `galay-utils/algorithm/trie.hpp`
- Modify or Move: `galay-utils/algorithm/mvcc.hpp`
- Modify or Move: `galay-utils/algorithm/huffman.hpp`
- Modify or Move: `galay-utils/config/*.hpp`
- Modify or Move: `galay-utils/app/app.hpp`
- Test: `test/data/*_test.cpp`, `test/app/*_test.cpp`, `test/config/*_test.cpp`
- Docs: `docs/02-API参考.md`, `docs/03-使用指南.md`

**Scope:**

Tighten data correctness, parsing errors, and CLI behavior without expanding into a full parser framework.

**Implementation Checklist:**

- Add trie tests for empty key, duplicate insert, remove prefix, remove missing key, Unicode byte-string behavior if documented, and full clear.
- Add MVCC tests for tombstone reads, CAS conflict, GC keep count, GC older-than, transaction double commit, and snapshot after delete.
- Add Huffman tests for single-symbol data, empty frequency map, invalid code length, missing symbol, truncated decode, and round-trip larger data.
- Add Config/INI/ENV/TOML parser tests for comments, whitespace, quoted values, arrays, dotted keys, invalid syntax, last error, and missing file.
- Add ParserManager tests for unknown extension, custom parser registration, and extension case handling if supported.
- Add App/Cmd tests for missing required args, default values, flag parsing, invalid typed values, subcommand dispatch, help output, and repeated args.

**Acceptance:**

- Parser tests assert both result and `lastError()` where an error is expected.
- CLI tests avoid relying on process-global state.
- Data structure docs state exception behavior and return conventions.

---

## Plan 7: Documentation, Examples, and Release Readiness

**Files:**
- Modify: `README.md`
- Modify: `docs/00-快速开始.md`
- Modify: `docs/01-架构设计.md`
- Modify: `docs/02-API参考.md`
- Modify: `docs/03-使用指南.md`
- Modify: `docs/04-示例代码.md`
- Modify: `docs/05-性能测试.md`
- Modify: `docs/06-高级主题.md`
- Modify: `docs/07-常见问题.md`
- Create or Move: `examples/include/<area>/*.cpp`
- Create or Move: `examples/import/<area>/*.cpp`
- Modify: `CHANGELOG.md` only when preparing a commit

**Scope:**

Make public documentation match the hardened module layout and provide usable examples for each supported group.

**Implementation Checklist:**

- Update README module list to match the target layout.
- Replace stale `System` time wording with `Time`.
- Add include examples for core, platform, concurrency, routing/resilience, data/config/app, and algorithms.
- Add import examples for the module facade where the toolchain supports it.
- Keep examples small and buildable; avoid examples that spawn destructive processes or install crash handlers.
- Update performance docs with benchmark build and run commands for every benchmark target.
- Update FAQ with directory layout, thread-safety, coroutine caveats, and compatibility-header policy.

**Acceptance:**

- `rtk cmake --build cmake-build-test` passes with examples enabled if the build tree enables examples.
- Docs do not mention removed include paths or old `System` time APIs.
- `CHANGELOG.md` summarizes the module hardening work before any commit.
