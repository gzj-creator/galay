# Async Etcd Benchmark Design

**Goal:** 为 etcd 模块新增真实可复现的异步 benchmark 入口，保留同步 benchmark 基线，并输出可直接对比的吞吐与延迟指标。

## 背景

当前仓库只有 [benchmark/etcd/b1_kv.cc](/Users/gongzhijie/Desktop/projects/git/galay/benchmark/etcd/b1_kv.cc)，它使用同步 `EtcdClient` 做多线程串行 `put`/`mixed(put+get)` 压测。这个结果可以作为同步基线，但不能体现 `AsyncEtcdClient` 接入最新 `galay-kernel` 后的真实异步性能。

## 设计目标

- 保留 `B1-EtcdKvBenchmark` 不动，继续作为同步基线。
- 新增 `B2-AsyncEtcdKvBenchmark`，使用 `AsyncEtcdClient`、`Runtime` 和 `IOScheduler` 实跑异步 KV 压测。
- 保持主要命令行参数和输出字段与 `B1` 尽量一致，方便直接横向对比。
- 先覆盖最常用的两种模式：
  - `put`
  - `mixed(put+get)`
- benchmark 结果必须来自真实运行，不写估算值。

## 方案选择

### 方案 A：新增独立异步 benchmark

新增 `B2-AsyncEtcdKvBenchmark`，保留 `B1` 不变。

优点：

- 同步与异步职责清晰
- 对比最直接
- 代码结构简单，后续容易再加 pipeline benchmark

缺点：

- 多一个 benchmark target

### 方案 B：把 `B1` 改成双模式

一个可执行文件通过参数切换 `sync|async`。

优点：

- 入口更少

缺点：

- 代码耦合高
- 同步/异步逻辑混在一起，不利于维护

### 结论

采用方案 A。

## 运行模型

- `B2` 使用 `RuntimeBuilder().ioSchedulerCount(N).computeSchedulerCount(0)` 创建运行时。
- 每个 benchmark worker 对应一个 coroutine 和一个 `AsyncEtcdClient` 实例。
- 每个 worker 仅 `connect()` 一次，结束时 `close()` 一次。
- worker 内部对自己的 `ops_per_worker` 任务串行执行，整体并发由多个 coroutine 并发推进。
- 默认模式下统计：
  - `Success`
  - `Failure`
  - `Duration`
  - `Throughput`
  - `Latency p50/p95/p99/max`

## 代码结构

- 新增 `benchmark/etcd/bench_support.h`
- 新增 `benchmark/etcd/bench_support.cc`
- 新增 `benchmark/etcd/b2_kv.cc`
- 新增 `test/etcd/t8_bench.cc`
- 更新 `benchmark/etcd/CMakeLists.txt`
- 更新 `test/etcd/CMakeLists.txt`

其中 support 组件负责：

- 参数结构与结果结构
- 聚合延迟和吞吐统计
- 启动 runtime 与 worker coroutine
- 收集 worker 成功/失败数据

## 测试策略

- 先写 `T8-AsyncBenchmarkSmoke`，以 `1 worker / 1 op` 运行 async benchmark support。
- 第一次运行应失败，因为 support/B2 尚未存在。
- 实现 support 后重新运行，确认测试转绿。
- 最后实跑：
  - `ctest --output-on-failure`
  - `./build-codex-etcd-awaitable/benchmark/etcd/B1-EtcdKvBenchmark`
  - `./build-codex-etcd-awaitable/benchmark/etcd/B2-AsyncEtcdKvBenchmark`

## 风险与控制

- 风险：异步 benchmark 如果共享状态处理不当，容易出现统计竞争。
  - 控制：每个 worker 独立记录 latency，汇总阶段再合并。
- 风险：benchmark 可能超时挂住。
  - 控制：给 runtime 执行加总超时，失败时统一计入错误并退出非零状态。
- 风险：async benchmark 只测到单连接串行，而不是更高 in-flight。
  - 控制：第一版先交付“多 coroutine + 多连接”的真实 async 基线，后续再扩成 pipeline / multi in-flight benchmark。
