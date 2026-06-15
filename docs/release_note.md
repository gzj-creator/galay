# Release Note

## v3.0.0 - 2026-06-15

- **版本级别**：大版本（major）
- **Git 提交消息**：`refactor: 将 src 模块命名空间化为 galay-* 前缀并补齐 MCP 客户端`
- **Git tag**：`v3.0.0`

### 变更摘要

本次为自 `v2.2.1` 以来的大版本重构发版，核心是将原有的单体 `galay/` 目录全面重构为按模块划分的 `src/` 多模块结构，并引入 Bazel 构建体系。本版本在初版发版后进一步将 `src/` 下各模块目录命名空间化为 `galay-` 前缀，归整内部目录，并将全部模块与测试/示例/基准的构建开关默认开启。

#### 架构与目录重构

- 将单体 `galay/` 目录重构为 `src/` 下 14 个独立模块：`etcd`、`http`、`http2`、`kernel`、`mcp`、`mongo`、`mysql`、`redis`、`rpc`、`ssl`、`tracing`、`utils`、`ws`。
- 将 `src/` 下各模块目录统一改为 `galay-` 前缀命名空间（`src/utils` → `src/galay-utils`、`src/kernel` → `src/galay-kernel` 等 13 个模块），内部 `kernel/kernel/` 改名为 `galay-kernel/core/`，全仓库 include 路径、测试、示例、基准同步适配。
- 解耦 `EventEngine` 与 `EventScheduler` 架构，移除 `TaskRunner`，将 `Holder` 重命名为 `CoSchedulerHandle`，`resume` 改为 `spawn`。
- 全面使用 `CoSchedulerHandle` 替换 `Runtime*` 参数。

#### 构建系统

- 重写 `CMakeLists.txt`，项目版本升至 `6.0.0`，按选项 `add_subdirectory` 各子模块并统一安装与包配置导出。
- `cmake/option.cmake` 中所有模块开关及 `BUILD_TESTING`、`GALAY_BUILD_EXAMPLES`、`GALAY_BUILD_BENCHMARKS` 默认改为 `ON`，开箱即构建完整套件。
- 新增 Bazel 构建支持：顶层 `BUILD.bazel`、`MODULE.bazel` 及各模块 `BUILD` 文件。
- 新增 `cmake/option.cmake`、`cmake/dependencies.cmake`、`cmake/galayConfig.cmake.in`。

#### 新功能

- 新增 MCP 客户端（`McpClient`，含 stdio / http 配置）及客户端表面与模式测试，补齐 mcp 模块客户端能力。
- 新增 `tracing` 链路追踪模块（span、sampler、exporter、OTLP、日志关联）。
- 新增 `io_uring` 支持。
- 新增 `UnsafeChannel`、单向 `LimitWaiter` 与协程清理功能。
- 新增 mpsc 异步队列及批处理支持。

#### 协程与并发优化

- `co_yield` 支持重新调度，移除 `suspend_choice`。
- 协程状态转化优化，增加协程锁实现互斥与同步。
- `AsyncChannel` 出队使用移动语义；ringbuffer 改造；mpsc 队列支持模板与批处理。
- 优化网络与文件 IO 事件的对象成员变量，减少每次调用接口的内存分配。

#### 文档与工程

- 新增 `docs/modules/` 模块文档体系，覆盖快速开始、架构设计、API 参考、使用指南、示例代码、性能测试、高级主题与常见问题。
- 新增 `examples/`、`scripts/` 目录。
- 大幅扩充 `.gitignore`，并将 `docs/plans/` 排除出版本控制。

#### 主要修复

- 修复 `coroutine.wait` 协程状态问题与协程状态竞态。
- 修复 mpsc 队列 batch 操作无法唤醒的 bug。
- 修复 `AsyncResult` 框架关键 bug。
- 修复 SSL 上下文管理，支持每个实例独立 ssl_ctx。
- 修复 io_uring 宏与 linux aio 事件的编译报错。
