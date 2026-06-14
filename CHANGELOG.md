# Changelog

本项目所有显著变更均会记录在本文件中。

## 如何维护

- **版本规则**：遵循 `major.minor.patch`。大改动（架构/目录重组/核心接口变更）升 `major`；新增功能升 `minor`；修复 bug、文档、配置、chore 等小修补升 `patch`。
- **更新时机**：每次提交前都必须更新本文件。未发版的变更写入 `## [Unreleased]` 节；发版时把 `Unreleased` 收束为新的版本节，并在最上方补一个空的 `## [Unreleased]`。
- **标题格式**：`## [vX.Y.Z] - YYYY-MM-DD`。
- **内容粒度**：按 `Added` / `Changed` / `Removed` / `Fixed` / `Docs` / `Chore` 等小节归纳，只记录最重要的变更，不逐行抄写 diff。

## [Unreleased]

## [v3.0.0] - 2026-06-14

### Changed

- **大版本重构**：将原有单体 `galay/` 目录重构为按模块划分的 `src/` 多模块结构，共 14 个模块：`etcd`、`http`、`http2`、`kernel`、`mcp`、`mongo`、`mysql`、`redis`、`rpc`、`ssl`、`tracing`、`utils`、`ws`。
- 重写顶层 `CMakeLists.txt`，项目版本升至 `6.0.0`，改为通过选项按需 `add_subdirectory` 各子模块，并增加统一的安装目标与 CMake 包配置导出。
- 解耦 `EventEngine` 与 `EventScheduler` 架构，移除 `TaskRunner`，将 `Holder` 重命名为 `CoSchedulerHandle` 并将其 `resume` 方法改为 `spawn`。
- 全面使用 `CoSchedulerHandle` 替换 `Runtime*` 参数，重构 `TcpServer`、`TcpSslServer`、`stress_tcp_client` 等组件。
- `co_yield` 支持重新调度并移除 `suspend_choice`；`UnsafeChannel` 支持 `size` 接口。
- 协程状态转化优化，增加协程锁实现互斥与同步，`co_yield` 支持暂停/不暂停两种状态。
- 优化网络与文件 IO 事件的对象成员变量，减少每次调用接口的内存分配。
- ringbuffer 改造，mpsc 队列支持模板与批处理以提升性能。
- `AsyncChannel` 出队使用移动语义；`AsyncResult` 每个类拥有各自的等待体。
- 大幅扩充 `.gitignore`，并将本地规划文档 `docs/plans/` 排除出版本控制。

### Added

- 新增 `tracing` 链路追踪模块（span、sampler、exporter、OTLP、日志关联等）。
- 新增 Bazel 构建支持：顶层 `BUILD.bazel`、`MODULE.bazel` 以及各模块内的 `BUILD` 文件。
- 新增 `cmake/option.cmake`、`cmake/dependencies.cmake`、`cmake/galayConfig.cmake.in`。
- 新增 `examples/` 示例目录与 `scripts/` 辅助脚本目录。
- 新增完整的 `docs/modules/` 模块文档体系。
- 新增 `io_uring` 支持。
- 新增 `UnsafeChannel`（仅支持同一 `CoSchedulerHandle` 内调用）。
- 新增单向 `LimitWaiter` 与协程清理功能。
- 新增 mpsc 异步队列及对应的压力测试用例。

### Removed

- 移除旧的单体目录 `galay/`（含 `algorithm`、`common`、`kernel`、`utils` 及其全部子目录）。
- 移除旧的 `test/`、`benchmark/`、`doc/` 目录与旧的 `README.md`、`README_CN.md`、`LICENSE`、`GalayConfig.cmake.in`。
- 移除 `CoroutineVisitor` 类。
- 移除 http2 相关旧函数。

### Fixed

- 修复 `coroutine.wait` 协程状态问题。
- 修复 mpsc 队列 batch 操作无法唤醒的 bug。
- 修复 `AsyncResult` 框架中的关键 bug。
- 修复 SSL 上下文管理与错误处理，每个 SSL 实例支持独立 ssl_ctx。
- 修复 io_uring 宏与 linux aio 事件的编译报错。
- 修复头文件依赖告警。
- 修复协程状态竞态问题。
