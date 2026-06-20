# Changelog

本项目所有显著变更均会记录在本文件中。

## 如何维护

- **版本规则**：遵循 `major.minor.patch`。大改动（架构/目录重组/核心接口变更）升 `major`；新增功能升 `minor`；修复 bug、文档、配置、chore 等小修补升 `patch`。
- **更新时机**：每次提交前都必须更新本文件。未发版的变更写入 `## [Unreleased]` 节；发版时把 `Unreleased` 收束为新的版本节，并在最上方补一个空的 `## [Unreleased]`。
- **标题格式**：`## [vX.Y.Z] - YYYY-MM-DD`。
- **内容粒度**：按 `Added` / `Changed` / `Removed` / `Fixed` / `Docs` / `Chore` 等小节归纳，只记录最重要的变更，不逐行抄写 diff。

## [Unreleased]

### Added

- 新增 HTTP/2 HEADERS-only 静态空响应 fast path，GET/HEAD exact path 命中时可绕过 active handler 和完整 stream 生命周期，并复用预编码响应头。
- 新增 HTTP/2 小 body 静态响应 bytes fast path，GET 命中时批量发送预编码 HEADERS 与 DATA bytes，HEAD 只返回响应头。
- 新增 HTTP/2 静态文件 metadata/cache 组件，支持 path 规范化防逃逸、404、小文件 body 缓存、MIME、ETag 与 If-None-Match 304。
- 新增 HTTP/2 静态空响应 h2load benchmark server 与 `scripts/http2_h2load_compare.sh --galay-static-empty` 模式，记录 req/s、p95、p99、CPU、RSS 与失败率。
- 新增 `scripts/http2_h2load_compare.sh --galay-static-small`，记录 1KB 静态响应 fast path 的 h2load 指标。
- 新增 HTTP/2 `H2StaticResponse`/`H2StaticRoute` 静态响应配置类型，以及 h2c/h2 server builder 的 `staticResponse()` 配置入口。
- 新增 `scripts/http2_h2load_compare.sh`，记录 galay h2c POST echo 与 `nghttpd --echo-upload` 的同参数外部 h2load 对比基线。
- 新增 HTTP/2 kernel 层 `flow_control` 发送窗口控制器，覆盖连接/stream 窗口消耗、WINDOW_UPDATE、SETTINGS_INITIAL_WINDOW_SIZE delta 与窗口溢出错误。
- 新增 HTTP/2 dispatcher/outbound scheduler 压力回归测试 `t85_h2pressure`，覆盖 1000 streams 公平调度、大 body 分片、频繁 WINDOW_UPDATE 与 GOAWAY 后新流拒绝。
- 新增 HTTP/2 kernel 压力基准 `benchmark_http2_h2_kernel_pressure`，分阶段输出 scheduler/bytes scheduler/flow control/dispatcher QPS 与瓶颈阶段。
- 新增 `Http2ConnectionCore::flushOutboundBytes()` 生产出站入口与 `Http2OutboundScheduler::pickSendableBytes(H2OutboundBudget, H2OutboundQueues&, ...)` 重载，control/headers 复用现有 frame 对象序列化、DATA 走 bytes 热路径，并补充 `t33_h2core` bytes flush 用例与 `b14_h2_kernel_pressure` core frame/bytes 压测场景。
- 新增 `galay-kernel/common/file_descriptor.h`，将 POSIX 文件描述符 RAII 封装迁移到 kernel 模块，并补充 kernel 边界测试覆盖打开失败、移动所有权与 release 语义。
- 新增 MCP 生产运行策略值类型 `mcp_policy.h`（传输资源限制、超时、HTTP 会话与认证策略，默认构造保持兼容），并补齐 `Timeout`/`Cancelled`/`Overload`/`Unauthorized`/`PayloadTooLarge` 错误码及 JSON-RPC 映射。
- 新增 MySQL RAII 连接租约 `MysqlPoolLease` 与 `acquireLease()` awaitable，借出连接在析构时自动归还，支持 `dismiss()` 转交所有权。
- 新增 Tracing span events/links 序列化支持，`file_span_exporter` 与 `otlp_http_exporter` 输出事件与链接，模块导出 `SpanEvent`/`SpanLink`。
- 新增 MongoDB replica set 拓扑、连接池、重试策略配置结构，以及 `mongodb://` URI 解析器 `parseMongoUri`，并扩展 `MongoConfig` 的 `seeds` seed list 字段。
- 新增对应测试：MCP 协议与策略默认值校验（t10）、Mongo URI 单测（t12）与 replica set 发现集成（t11）、MySQL 集成开关（t15）与 RAII 租约集成（t16）、Tracing span events/links（t12）与 tracer provider（t13）。
- 新增基准：MCP 策略默认值 smoke（b4）、MySQL 异步连接池租约压力（b4）。
- Redis、Mongo、MySQL 集成测试统一在 CTest 注册带标签测试名，便于按标签过滤运行。
- 新增 MySQL 真实服务端认证插件矩阵集成测试，覆盖 `mysql_native_password`、`caching_sha2_password` 成功路径与 `sha256_password` 不支持路径，并补充本机/CI 用户准备脚本和验证文档。
- 新增 MySQL 真实服务端连接恢复测试，覆盖错误端口连接失败后的恢复，以及服务端 `KILL CONNECTION` 后的新连接恢复。
- 新增 MySQL 异步连接池协程来源检查、等待者唤醒集成测试与连接池压力基准，覆盖连接池无阻塞等待路径。
- 新增 etcd、Mongo、Redis 真实服务端验证脚本，并补齐 Redis ACL 认证兼容测试。
- 新增 Redis 拓扑客户端读路由、重试、刷新配置与统计快照接口，并覆盖普通 Redis 与 Rediss 主从/集群构建路径。
- 新增 tracing 进程级 `SpanProcessor` 配置入口与 `SpanProcessorScope`，SpanGuard 结束采样 span 时可提交给当前处理器。

### Changed

- HTTP/2 静态文件普通 200 GET/HEAD fast path 改为使用 `lookupFast200()` 轻量查询，直接复用预编码 200 响应头与共享 body，避免构造完整 lookup 和扫描 `content-length`。
- HTTP/2 静态文件 GET fast path 改为优先使用轻量 HPACK request target 解析，并携带 If-None-Match/Range 目标头，命中静态文件时避免全量 header vector 解码。
- HTTP/2 静态文件 GET fast path 复用预编码响应头、连接私有静态文件 cache 与共享 DATA payload，减少静态文件路径中的 HPACK 编码、文件路径规范化和 body 拷贝成本。
- HTTP/2 h2c accept 后的连接处理改为轮询分发到 IO scheduler，避免 macOS loopback/SO_REUSEPORT 哈希倾斜时压测连接集中到单个 worker。
- HTTP/2 `frame_disp` 升级为 typed result/action 模型，补齐 frame stream id 约束、CONTINUATION 序列、WINDOW_UPDATE 0 增量、最小 stream lifecycle 与 GOAWAY 后新流拒绝策略。
- HTTP/2 `out_sched` 改为 pending chunk queue + Deficit Round Robin 调度，避免 `std::sort(streams)` 改变调用方顺序和 `std::string::erase(0, chunk)` 搬移大 body，并新增 DATA bytes 调度路径提升热路径吞吐。
- HTTP/2 `h2_core` 增加事件驱动入口、出站队列 flush、显式 Draining/Closing 状态和 typed core error 边界，减少常规发送对固定 tick 的依赖。
- 统一 HTTP/HTTP2 内部头文件命名：`reader_cfg.h`/`writer_cfg.h` 改为 `reader_settings.h`/`writer_settings.h`，`static_cfg.h` 改为 `file_settings.h`，`stream_mgr.h` 改为 `stream_manager.h`，并同步更新源码、示例、测试与文档引用。
- MySQL 同步与异步客户端认证流程改为按当前认证插件状态循环处理，支持服务端 `AuthSwitchRequest` 后重算认证响应，并保留 `caching_sha2_password` fast/full auth 流程。
- MySQL 异步连接池改用无锁队列与协程 waker 管理空闲连接和等待者，避免在协程获取连接路径中使用阻塞同步。
- etcd 与 Redis 集成测试中的轮询等待改为协程等待/睡眠，减少 IO 调度线程上的阻塞等待。
- 移除旧协程任务别名与相关命名，统一改用 `Task<void>` 表达异步任务接口。
- 继续整理剩余命名、注释与示例表述，统一任务与句柄相关术语，并同步修正文档。
- 调整 HTTP、Mongo、Redis 集成测试构建规则，按可选模块和集成测试二进制需求决定目标注册与构建。

### Removed

- 移除 `galay-http/server/file_descriptor.h`，HTTP 静态文件发送路径改为直接使用 kernel 层 `FileDescriptor`。

### Fixed

- 修复 HTTP/2 静态文件 cache 共享可变状态在多 IO worker 下需要加锁的问题，改为每连接克隆 cache；同时归一化 query path cache key，避免长连接通过 query variant 放大缓存条目。
- 修复 CMake OpenSSL 探测在 Homebrew 升级后复用失效 Cellar cache，导致 `openssl/err.h` 找不到的问题。
- 修复 WSS `echoLoopConsume()` 状态机移动后仍持有旧对象 `message`/`opcode` 指针的问题，避免 WSS benchmark 服务端在回显循环进入下一轮读取时段错误。
- 修复 MySQL 8 默认 `caching_sha2_password` 握手遇到 `mysql_native_password` 账号时返回 auth switch 导致连接失败的问题。
- 补齐 MySQL 认证插件分发、AuthSwitchRequest 边界解析与 RSA full auth 负例测试；修复 Redis 协议测试在新编译器下缺少 `<cstring>` 的编译问题。
- 修复 `MurmurHash3Util` 字符串字面量重载误匹配裸指针长度接口导致的越界读取。
- 修复 kernel work-stealing、kqueue IO 完成/超时清理中的生命周期问题，避免 stale IO 注册与任务状态悬空访问。
- 稳定 `kernel.stealstats` 与 `kernel.connbld` 测试同步条件，消除 sanitizer 和高并发建连场景下的时序误失败。
- 修复 epoll IO 事件完成后 awaitable/注册入口清理不完整导致的晚到事件、复用 fd 与序列 IO 生命周期风险。
- 修复跨线程注入任务 stealing 在 owner 首次搬运前抢占任务的问题，并在定时器调度器停止后清空遗留定时器。
- 加固 etcd/Redis 服务端验证脚本，跳过未构建示例、等待 Redis 集群达到预期规模，并为集成测试显式启用运行开关。

### Docs

- 补充 HTTP/2 静态文件 Release 对比校正文档，明确非 Release 构建不能与 Homebrew `nghttpd` 发布版作公平性能结论，并记录 0B/1KB 静态文件同参数 h2load 对照。
- 新增 HTTP/2 性能测试文档，记录 kernel 压测环境、复现命令、QPS/MiB/s 指标、真实瓶颈和后续优化方向。
- 新增 HTTP/2 dispatcher/scheduler 生产级优化计划并按任务勾选执行进度。
- 新增 MySQL 认证插件真实服务端验证说明，记录本机测试用户创建、集成测试运行和清理流程。
- 新增 `CLAUDE.md` 与 `AGENTS.md`，定义 LLM 代理在本仓库内的行为准则（编码前先思考、简洁优先、外科手术式修改、目标驱动执行），降低常见编码错误。

### Chore

- 新增 `scripts/mysql/mysql_auth_matrix_setup.sh`，按模块目录管理 MySQL auth 矩阵测试用户准备脚本。
- 扩充 `.gitignore`，新增 `.claude/`、`.codex/` 条目，避免代理本地配置目录进入版本控制。
- 扩充 `.gitignore`，新增 `docs/modules/*/plans`，避免按模块拆分的本地规划文档进入版本控制。
- 移除 examples/tests/benchmarks/scripts style 审计中的 `stale-include-root` 阻断规则，保留其他结构与命名检查。

## [v3.0.0] - 2026-06-15

### Changed

- **模块命名空间化**：将 `src/` 下各模块目录统一改为 `galay-` 前缀命名空间（`src/utils` → `src/galay-utils`、`src/kernel` → `src/galay-kernel` 等 13 个模块），消除模块名与公共短名冲突，便于安装布局与包依赖区分。
- **内部目录归整**：`kernel/kernel/` 改名为 `galay-kernel/core/`，全仓库 include 路径（`kernel/kernel/runtime.h` → `galay-kernel/core/runtime.h`）与测试、示例、基准同步适配。
- **默认构建策略调整**：`cmake/option.cmake` 中所有模块开关（`GALAY_BUILD_SSL`、`GALAY_BUILD_HTTP` 等）及 `BUILD_TESTING`、`GALAY_BUILD_EXAMPLES`、`GALAY_BUILD_BENCHMARKS` 默认改为 `ON`，开箱即构建完整套件。
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
- **测试构建改造**：统一各测试模块 CTest 命名为 `<module>.<scenario>` 场景名（剥离 `tNN_` 前缀），替代原目标名/文件名，并新增 `cmake/RunTestBinary.cmake` 测试二进制运行辅助脚本。
- **测试路径解析**：kernel 测试引入 `GALAY_PROJECT_ROOT` / `GALAY_SOURCE_ROOT` 编译宏，源码对齐类测试改用编译期宏解析工程路径，替代基于 `__FILE__` 的运行时路径推算，并同步适配 `kernel/kernel/` → `galay-kernel/core/` 等模块结构调整后的路径。
- mcp 测试改用显式源文件列表替代 `file(GLOB)`，移除过时的 stdio/http 集成测试（`t1_stdio`、`t2_stdio`、`t3_http`、`t4_http`）；移除 `t94` 中已失效的 iocp / concurrentqueue / Bazel alias 断言。

### Added

- 新增 MCP 客户端（`McpClient`，含 stdio / http 两种配置）及对应客户端表面与模式测试（`t8_client_surface`、`t9_client_mode`），补齐 mcp 模块的客户端能力。
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
- 移除旧访问器类。
- 移除 http2 相关旧函数。

### Fixed

- 修复 `coroutine.wait` 协程状态问题。
- 修复 mpsc 队列 batch 操作无法唤醒的 bug。
- 修复 `AsyncResult` 框架中的关键 bug。
- 修复 SSL 上下文管理与错误处理，每个 SSL 实例支持独立 ssl_ctx。
- 修复 io_uring 宏与 linux aio 事件的编译报错。
- 修复头文件依赖告警。
- 修复协程状态竞态问题。

### Docs

- 新增项目 `README.md`，介绍 galay 特性、13 个 `galay-*` 模块、环境要求、CMake 快速开始与目录结构。
