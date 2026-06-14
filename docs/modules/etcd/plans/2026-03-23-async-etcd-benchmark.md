# Async Etcd Benchmark Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 为 etcd 模块新增异步 benchmark 入口，并用真实实跑数据对比同步与异步性能。

**Architecture:** 保留现有同步 benchmark 作为基线，新增一个异步 benchmark target。异步 benchmark 的可复用执行逻辑抽到 benchmark support 文件中，并由一个最小 smoke test 先行驱动实现，避免只测构建不测行为。

**Tech Stack:** CMake, C++23, `AsyncEtcdClient`, `Runtime`, `IOScheduler`, `ctest`

---

### Task 1: 写异步 benchmark smoke test

**Files:**
- Create: `test/etcd/t8_bench.cc`
- Modify: `test/etcd/CMakeLists.txt`
- Test: `test/etcd/t8_bench.cc`

**Step 1: Write the failing test**

测试目标：

- 构造 `1 worker / 1 op / put` 的 async benchmark 参数
- 调用尚未实现的 benchmark support 入口
- 断言：
  - `success == 1`
  - `failure == 0`
  - `latency_samples == 1`
  - `throughput > 0`

**Step 2: Run test to verify it fails**

Run: `cmake --build build-codex-etcd-awaitable -j8 --target T8-AsyncBenchmarkSmoke`

Expected: FAIL，因为 benchmark support 头或实现尚不存在。

**Step 3: Write minimal implementation**

新增 benchmark support 的最小可执行骨架和返回结构，先让 smoke test 能链接并跑通。

**Step 4: Run test to verify it passes**

Run: `./build-codex-etcd-awaitable/test/etcd/T8-AsyncBenchmarkSmoke`

Expected: PASS

**Step 5: Commit**

实现完成后再统一提交，不在此步单独提交。

### Task 2: 实现 `B2-AsyncEtcdKvBenchmark`

**Files:**
- Create: `benchmark/etcd/bench_support.h`
- Create: `benchmark/etcd/bench_support.cc`
- Create: `benchmark/etcd/b2_kv.cc`
- Modify: `benchmark/etcd/CMakeLists.txt`
- Test: `benchmark/etcd/b2_kv.cc`

**Step 1: Write the failing test**

以 `Task 1` 的 smoke test 作为红灯，确保 benchmark support 行为先被覆盖。

**Step 2: Run test to verify it fails**

Run: `cmake --build build-codex-etcd-awaitable -j8 --target T8-AsyncBenchmarkSmoke`

Expected: FAIL

**Step 3: Write minimal implementation**

实现：

- async benchmark 参数解析
- runtime 建立与调度
- 每 worker 一个 `AsyncEtcdClient`
- 支持 `put` 和 `mixed`
- 输出与 `B1` 对齐的统计项

**Step 4: Run test to verify it passes**

Run:

- `cmake --build build-codex-etcd-awaitable -j8`
- `./build-codex-etcd-awaitable/test/etcd/T8-AsyncBenchmarkSmoke`

Expected: PASS

**Step 5: Commit**

实现完成后再统一提交，不在此步单独提交。

### Task 3: 实跑验证并给出同步/异步对比

**Files:**
- Modify: `docs/05-性能测试.md`

**Step 1: Write the failing test**

这里不新增代码测试，直接做真实验证。

**Step 2: Run test to verify it fails**

不适用。

**Step 3: Write minimal implementation**

补充文档，说明 `B2` 的参数、运行模型和结果发布要求。

**Step 4: Run test to verify it passes**

Run:

- `ctest --output-on-failure`
- `./build-codex-etcd-awaitable/benchmark/etcd/B1-EtcdKvBenchmark http://127.0.0.1:2379 8 500 64 put`
- `./build-codex-etcd-awaitable/benchmark/etcd/B2-AsyncEtcdKvBenchmark http://127.0.0.1:2379 8 500 64 put`

Expected:

- tests 全绿
- 两个 benchmark 均成功输出
- 能基于真实结果比较 sync/async 吞吐与延迟

**Step 5: Commit**

验证完成后统一提交，并在最终汇报中注明所有数字都来自实跑。
