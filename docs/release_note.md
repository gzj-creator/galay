# Release Note

## v3.0.0 - 2026-06-22

- **版本级别**：大版本（major）
- **Git 提交消息**：`feat: 新增 kernel TCP accept C ABI 异步接口并配套 expected 错误边界`
- **Git tag**：`v3.0.0`

### 变更摘要

本次为自 `v2.2.1` 以来的大版本重构发版，核心是将原有的单体 `galay/` 目录全面重构为按模块划分的 `src/` 多模块结构，并引入 Bazel 构建体系。本版本在初版发版后又进一步将 `src/` 下各模块目录命名空间化为 `galay-` 前缀，归整内部目录，并将全部模块与测试/示例/基准的构建开关默认开启；同期统一了测试 CTest 场景命名、改用编译宏解析工程路径，并新增项目 README。随后在此版本内持续推进 HTTP/2 静态文件快路径与 kernel 调度/流控的生产级打磨，补齐 MCP、MySQL、Mongo、Redis、Tracing 等客户端模块的生产策略与真实服务端集成验证，最终将 tag 与 Release 移至最新提交以收口本版本全部累计变更。

### v3.0.0 tag 移至 2026-06-22 的增量

本次 tag 移动主要吸收自初版 v3.0.0 以来的以下增量：

- **kernel TCP accept C ABI 异步接口**：新增 `galay_kernel_tcp_accept_{start,wait,join,destroy}` 以及 `galay_kernel_tcp_socket_{bind,listen,local_endpoint}`，通过 runtime 调度的 `JoinHandle<AcceptResult>` 暴露异步 accept，并经 peer/local host config 返回 IPv4/IPv6 地址与端口；配套新增 `test/c/kernel/t4_tcp_accept_api`、`examples/c/kernel/e2_tcp_accept`、`benchmark/c/kernel/b2_tcp_accept_smoke`。
- **底层启动/创建边界改为 `std::expected`**：`TcpSocket::create` / `UdpSocket::create` 新增返回 `std::expected` 的静态工厂；`Scheduler::start` / `Runtime::start` / `ensureStarted` / `acquireDefaultScheduler` 改为 `std::expected` 返回，新增 `RuntimeErrorCode::kSchedulerStartFailed`，启动失败自动 `stop()` 已启动的 scheduler。
- **Reactor 抽象改为 `ReactorType` concept**：移除虚基类 `BackendReactor`，改为基于 `notify()` + `getHandle()` 的编译期约束，epoll/kqueue/io_uring 三后端通过 `static_assert` 锁定；新增 `Reactor::start()` 显式初始化。
- **IPv6 dual-stack**：`HandleOption::handleIPv6Only(bool)` 显式设置 `IPV6_V6ONLY`，`TcpSocket::openHandle` / `UdpSocket::openHandle` 在 IPv6 场景默认调用 `handleIPv6Only(false)` 启用 dual-stack；新增 `test/cpp/kernel/t127_ipv6only`。
- **C ABI tcp/udp socket create 移除 try/catch**：直接消费新 `create()` 工厂返回的 `IOError`。
- **rpc 生产级能力与 etcd 服务发现**：补齐调用级 metadata/options、`RpcChannel`、deadline/cancel、heartbeat/reconnect、连接池、托管客户端、重试/治理/背压等公共能力，并在启用 `galay-etcd` 时走真实 etcd v3 KV 作为注册中心。
- **源码/测试分层与 C ABI 用例**：`src/` 下各模块统一迁入 `src/cpp/`；benchmark/examples/test 按语言分层，新增 `benchmark/c/`、`examples/c/`、`test/c/`（99 个文件）作为 C ABI 回归入口；通过 `GALAY_BUILD_C_API=ON` 启用。
- **协程与并发原语加固**：修复 `AsyncWaiter` / `AsyncMutex` / `MpscChannel` / `WaitRegistration` 的 await_suspend 竞态与丢失唤醒问题；修复 `RpcClient::call` / `RpcManagedClient::call` 在挂起协程中持有栈上 borrowed payload 的 use-after-free；修复 RPC stream 控制帧 body 校验。

具体代码变更与边界用例细节请参见 `CHANGELOG.md` 的 `v3.0.0` 版本节。

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

#### HTTP/2 静态文件快路径与 kernel 调度

- 新增 HTTP/2 HEADERS-only 静态空响应 fast path、小 body bytes fast path 与静态文件 metadata/cache 组件（path 规范化防逃逸、404、小文件 body 缓存、MIME、ETag 与 If-None-Match 304），并新增 `H2StaticResponse`/`H2StaticRoute` 配置类型与 `staticResponse()` 配置入口。
- HTTP/2 静态文件普通 200 GET/HEAD fast path 改用 `lookupFast200()` 轻量查询、轻量 HPACK request target 解析，并复用预编码响应头、连接私有 cache 与共享 DATA payload，降低 HPACK 编码与 body 拷贝成本。
- 新增 HTTP/2 kernel 层 `flow_control` 发送窗口控制器，覆盖连接/stream 窗口消耗、WINDOW_UPDATE、SETTINGS_INITIAL_WINDOW_SIZE delta 与窗口溢出错误。
- HTTP/2 `frame_disp` 升级为 typed result/action 模型，补齐 frame stream id 约束、CONTINUATION 序列、WINDOW_UPDATE 0 增量、最小 stream lifecycle 与 GOAWAY 后新流拒绝策略。
- HTTP/2 `out_sched` 改为 pending chunk queue + Deficit Round Robin 调度，新增 DATA bytes 调度路径；`h2_core` 增加事件驱动入口、出站队列 flush、显式 Draining/Closing 状态与 typed core error 边界。
- h2c accept 后的连接处理改为轮询分发到 IO scheduler，避免 macOS loopback/SO_REUSEPORT 哈希倾斜时连接集中到单个 worker。
- 新增 `Http2ConnectionCore::flushOutboundBytes()` 出站入口与 `pickSendableBytes` bytes 重载，补充 dispatcher/scheduler 压力回归测试 `t85_h2pressure` 与 kernel 压力基准。

#### 客户端模块生产级能力

- 新增 MCP 客户端（`McpClient`，含 stdio / http 配置）及生产运行策略 `mcp_policy.h`，补齐 `Timeout`/`Cancelled`/`Overload`/`Unauthorized`/`PayloadTooLarge` 错误码及 JSON-RPC 映射。
- 新增 MySQL RAII 连接租约 `MysqlPoolLease` 与 `acquireLease()` awaitable；异步连接池改用无锁队列与协程 waker 管理空闲连接和等待者；认证流程支持 `AuthSwitchRequest` 与 `caching_sha2_password` fast/full auth。
- 新增 MongoDB replica set 拓扑、连接池、重试策略配置结构与 `mongodb://` URI 解析器 `parseMongoUri`，扩展 `MongoConfig` seed list。
- 新增 Redis 拓扑客户端读路由、重试、刷新配置与统计快照接口，覆盖普通 Redis 与 Rediss 主从/集群构建路径。
- 新增 Tracing span events/links 序列化、进程级 `SpanProcessor` 配置入口与 `SpanProcessorScope`。

#### 协程与并发优化

- `co_yield` 支持重新调度，移除 `suspend_choice`。
- 协程状态转化优化，增加协程锁实现互斥与同步。
- `AsyncChannel` 出队使用移动语义；ringbuffer 改造；mpsc 队列支持模板与批处理。
- 优化网络与文件 IO 事件的对象成员变量，减少每次调用接口的内存分配。
- 新增 `UnsafeChannel`、单向 `LimitWaiter` 与协程清理功能。

#### 测试基础设施与真实服务端集成

- 统一各测试模块 CTest 命名为 `<module>.<scenario>` 场景名（剥离 `tNN_` 前缀），新增 `cmake/RunTestBinary.cmake` 运行辅助脚本。
- kernel 测试引入 `GALAY_PROJECT_ROOT` / `GALAY_SOURCE_ROOT` 编译宏，源码对齐类测试改用编译期宏解析工程路径。
- mcp 测试改用显式源文件列表替代 `file(GLOB)`，移除过时的 stdio/http 集成测试与失效断言。
- Redis、Mongo、MySQL 集成测试统一在 CTest 注册带标签测试名；新增 MySQL 认证插件矩阵、连接恢复、连接池协程来源与等待者唤醒集成测试。
- 新增 etcd、Mongo、Redis、MySQL 真实服务端验证脚本与对应文档。

#### 文档与工程

- 新增项目 `README.md`，介绍 galay 特性、13 个 `galay-*` 模块、环境要求、CMake 快速开始与目录结构。
- 新增 `docs/cpp/modules/` 与 `docs/c/modules/` 模块文档体系，覆盖快速开始、架构设计、API 参考、使用指南、示例代码、性能测试、高级主题与常见问题。
- 新增 `examples/`、`scripts/` 目录；新增 `CLAUDE.md` 与 `AGENTS.md` 代理行为准则。
- 新增 HTTP/2 静态文件 Release 对比校正与性能测试文档，新增 HTTP/2 dispatcher/scheduler 生产级优化计划，新增 MySQL 认证插件验证说明。
- 大幅扩充 `.gitignore`，并将 `docs/plans/`、`.claude/`、`.codex/`、`docs/cpp/modules/*/plans` 排除出版本控制。

#### 主要修复

- 修复 `coroutine.wait` 协程状态问题与协程状态竞态。
- 修复 mpsc 队列 batch 操作无法唤醒的 bug。
- 修复 `AsyncResult` 框架关键 bug。
- 修复 SSL 上下文管理，支持每个实例独立 ssl_ctx。
- 修复 io_uring 宏与 linux aio 事件的编译报错。
- 修复 HTTP/2 静态文件 cache 多 worker 共享可变状态、HPACK 全量解码与 query variant 缓存放大问题。
- 修复 WSS 回显循环状态机移动后悬空指针、CMake OpenSSL 探测失效 Cellar cache、MySQL 8 auth switch、`MurmurHash3Util` 越界读取等问题。
- 修复 epoll/kqueue/kernel work-stealing 中的 IO 完成清理、awaitable 生命周期与跨线程任务抢占问题。
