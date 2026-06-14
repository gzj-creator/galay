# galay-redis Rust 客户端同机同参 Benchmark 对齐设计

## 背景

当前 `galay-redis` 已经完成：

- awaitable 体系向 `Builder + StateMachine` 收敛
- `rediss://` / TLS 客户端能力落地
- TLS 命令收发热路径切到 `galay-ssl` 原生状态机
- 基础 benchmark (`b1` / `b3`) 能稳定输出 plain/TLS 的 normal 与 pipeline 吞吐

下一步目标不是继续盲目调优，而是先回答一个更关键的问题：

- 在同一台机器、同一个 Redis、同一组 workload 参数下，`galay-redis` 和主流 Rust Redis 客户端相比到底差多少？
- 差距来自 TLS 开销、协议编码解析、请求模型，还是来自 Rust 客户端特有的 multiplexing / automatic pipelining 架构？

为此需要建立一套“同机同参、同模型”的公平对齐基准，并额外保留一组“各家原生最佳实践上限”的参考数据。

## 目标

### 功能目标

- 在本仓库内新增一套可重复运行的 Rust 对照 benchmark
- 覆盖两类主流 Rust 客户端：
  - `redis-rs`
  - `fred.rs`
- 覆盖四组核心 workload：
  - `plain normal`
  - `plain pipeline`
  - `TLS normal`
  - `TLS pipeline`
- 在同一套参数下输出与现有 `b1` / `b3` 尽量一致的指标

### 分析目标

- 输出 `Same-model baseline` 对照表
- 输出 `Native ceiling reference` 对照表
- 明确 `galay-redis` 的差距主要落在哪一层：
  - 连接模型
  - pipeline 模型
  - TLS 路径
  - 编码/解析
  - 调度模型

## 约束与原则

### 1. 同机同参优先于“官方自带 benchmark”

Rust 客户端仓库自带 benchmark 可以作为补充参考，但不能直接拿来与本仓库 `b1/b3` 对表。原因是：

- `fred.rs` 自带 benchmark 默认更偏向共享 client、高 Tokio 并发和 automatic pipelining
- `redis-rs` 自带 benchmark 同时覆盖普通连接、pipeline 和 multiplexed async 模型
- 它们的默认命令种类、连接共享方式、并发策略、输出口径都与当前 `b1/b3` 不一致

因此必须优先实现本仓库内部的 Rust 最小 benchmark 程序，以保证 workload 一致。

### 2. 数据分成两层

#### Same-model baseline

所有客户端都尽可能按相同模型运行：

- 同一台机器
- 同一个 Redis 实例
- 同一套 TLS 证书
- 同样的 key/value 构造
- 同样的 clients / operations / batch size / timeout
- 每个 worker 自己持有连接
- pipeline 只在显式 pipeline workload 中启用
- 禁用或绕开会破坏公平性的自动聚合能力

它回答的问题是：

- 在同样模型下，`galay-redis` 与 `redis-rs` / `fred.rs` 的纯实现差距有多大？

#### Native ceiling reference

保留各家推荐用法的额外参考组：

- `redis-rs` 的 `MultiplexedConnection`
- `fred.rs` 的共享 client / implicit or automatic pipelining 路线
- `galay-redis` 当前最优推荐用法

它回答的问题是：

- 如果允许各家发挥自己的架构优势，理论上还能把上限拉到哪里？

## Workload 设计

### 1. plain normal

- 单连接
- worker 内串行执行
- 每轮：
  - `SET key value`
  - `GET key`
- 不共享连接
- 不使用隐式 pipeline

### 2. plain pipeline

- 单连接
- 显式 pipeline
- 每批仅发送 `SET`
- 默认 `batch_size = 50`
- 不共享连接

### 3. TLS normal

与 `plain normal` 完全相同，只把传输切换为：

- `rediss://localhost:16380/0`

### 4. TLS pipeline

与 `plain pipeline` 完全相同，只把传输切换为 TLS。

## 参数档位

先固定两档：

### smoke

- `clients = 10`
- `operations = 200`
- `batch_size = 50`

用途：

- 快速验证功能与短跑开销
- 看小样本下调度、握手和 pipeline 初始化成本

### steady

- `clients = 10`
- `operations = 5000`
- `batch_size = 50`

用途：

- 看稳定吞吐
- 更贴近“真实瓶颈在哪”的结论

## 统一输出

所有 benchmark 尽量统一输出：

- `Success`
- `Error`
- `Timeout`
- `Duration`
- `Ops/sec`
- `Request latency p50`
- `Request latency p99`

输出格式应尽量贴近当前 `b1` / `b3`，便于直接横向对比和后续脚本汇总。

## Rust 客户端边界

### redis-rs baseline

- `normal`
  - 使用独立 async connection
  - 每个 worker 一条连接
  - 串行 `SET + GET`
- `pipeline`
  - 使用独立 async connection
  - 显式 `Pipeline`
  - 每批 `SET x50`
- baseline 中不使用 `MultiplexedConnection`

### fred.rs baseline

- `normal`
  - 采用尽量保守的独立连接模型
  - 每个 worker 使用独立 client/connection
- `pipeline`
  - 只使用显式 pipeline
  - 尽量避免依赖它的 automatic pipelining 改变模型

### Native ceiling

- `redis-rs`
  - 增加 `MultiplexedConnection` 参考组
- `fred.rs`
  - 增加共享 client / 自动 pipeline 参考组

## 目录与产出

建议新增：

- `benchmarks/rust/redis-rs-bench/`
- `benchmarks/rust/fred-bench/`
- `benchmarks/rust/run_rust_alignment.sh`

每个 Rust 基准程序都应：

- 支持与 `b1/b3` 尽量一致的参数接口
- 同时支持 plain 和 TLS
- 支持 normal 与 pipeline 模式

总控脚本负责：

- 运行 baseline 四组
- 运行 ceiling 参考组
- 汇总结果

## 验收标准

本轮工作的完成标准是：

1. Rust baseline 程序能在本机稳定运行四组 workload
2. Rust ceiling 参考组能单独运行
3. 有一份可复现的运行脚本
4. 能产出一份最终对照结论，明确：
   - `galay-redis` 相比 `redis-rs`
   - `galay-redis` 相比 `fred.rs`
   - plain/TLS、normal/pipeline 各自差距
   - 下一轮优化的优先方向
