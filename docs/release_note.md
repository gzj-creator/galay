# Release Note

## v4.0.1 - 2026-07-05

- **版本级别**：修订版本（patch）
- **Git 提交消息**：`chore: 发版 v4.0.1（移动 tag 至最新提交，补齐累计变更说明）`
- **Git tag**：`v4.0.1`（force-move 至本次发布提交）

### 变更摘要

本次为 `v4.0.0` 之后的修订版本发版，并把 `v4.0.1` tag 从初始发布提交移动到最新发布提交，使其覆盖 `v4.0.0 → HEAD` 的全部累计变更。主干内容如下。

- **新增 galay-framework 开发 skill**（`agent/skill/galay-usage/`）：作为在 galay 上开发服务端 / 客户端 / 中间件与 C/FFI 的统一入口；`SKILL.md` 与 `references/{cpp-api,c-api}.md` 覆盖 Runtime / `Task<T>` / `std::expected` 心智模型、include 前缀与命名空间、CMake 链接 target、构建开关、平台后端速查与 13 个 C++ 模块 + C ABI 模块地图。顶层 `SKILL.md` 与 `references/` 已迁入 `agent/skill/galay-usage/` 命名目录，便于安装与复用。
- **统一 CMake 安装包为单一 `galay` 包**：移除顶层 `CMakeLists.txt` 按模块生成独立 package config 的 foreach，删除模板 `cmake/galay-module-config.cmake.in`；安装后只在 `lib/cmake/galay` 下导出 `galayConfig.cmake` / `galayConfigVersion.cmake` / `galayTargets.cmake`，外部项目统一通过 `find_package(galay CONFIG REQUIRED)` 后按需链接 `galay::<module>`，并由 install 布局校验断言不再安装按模块的 package 目录。
- **全模块新增 TCP_NODELAY 可配置化**：etcd / http / http2 / mcp / mysql / redis / rpc / ws 各模块客户端与服务端配置新增 `tcp_no_delay` 字段与 `tcpNoDelay()` builder，连接建立或 accept 后按配置启用 `TCP_NODELAY`，选项失败按各模块语义显式传播。
- **kernel 写路径改用局部 SIGPIPE 抑制**：`handleWritev` 改为 `sendmsg()` + `MSG_NOSIGNAL`，与 `handleSend` / `handleSendTo` 对齐；支持平台默认启用 `SO_NOSIGPIPE`，框架不再依赖全局 SIGPIPE 处置，向断连 socket 写入返回 `EPIPE`。
- **修复 HTTP server 路由参数丢失**：路由命中后立即把解析出的路径参数回填到 `request`，业务 handler 现在能正确读取 `/users/:id` 这类路径参数。
- **全模块性能测试文档与基准数据落地**：为 kernel / http / http2 / ws / rpc / ssl / tracing / utils / redis / mysql / mongo / etcd 各模块补全 `05-性能测试.md`，并归档 `benchmark_data/`、`configs/` 压测结果与复现配置；新增 `docs/benchmark_plan.md` 与 `docs/machine_config.md`。
- **构建与脚本整理**：`BUILD_TESTING` / `GALAY_BUILD_EXAMPLES` / `GALAY_BUILD_BENCHMARKS` 默认改为 `OFF`（按需开启）；`scripts/` 按模块归类并加数字前缀；构建版本号（`CMakeLists.txt` 与 `MODULE.bazel`）对齐 `4.0.1`。
- **清理过期文档**：删除被新 skill 取代的旧优化建议、陈旧 `docs/release_note.md`（仅停留在 v3.0.0）与 `docs/README.md` 等。

## v4.0.2 - 2026-07-06

- **版本级别**：小版本（patch）
- **Git 提交消息**：`docs: 补全 C ABI 模块文档并对齐 README 目录结构`
- **Git tag**：`v4.0.2`

### 变更摘要

本次为 `v4.0.1` 之后的小版本发版，主线为文档：补全 C ABI 模块文档、对齐 README 目录结构说明并清理过期压测计划文档，无代码改动。

- **补全 C ABI 模块文档**：在 `docs/c/modules/` 下新增 `bridge` / `common` / `utils` 共享层 README，给出源码位置、CMake target / alias、依赖与主要职责，与既有各协议模块（kernel / http / http2 / ws / rpc / ssl / tracing / redis / mysql / mongo / etcd / mcp）文档对齐。
- **对齐 README 目录结构**：把“模块化构建”说明拆分为 C++ 模块（`src/cpp/galay-*`）与 C ABI 模块（`src/c/galay-*-c`），仓库目录树同步区分 `docs/cpp/modules/` 与 `docs/c/modules/`，并补上 C ABI 文档入口指引。
- **清理过期文档**：删除 `docs/benchmark_plan.md`，其内容已被 v4.0.1 各模块 `05-性能测试.md` 与归档基准数据取代。

## v4.1.0 - 2026-07-10

- **版本级别**：次版本（minor）
- **Git 提交消息**：`chore: 发版 v4.1.0 并对齐构建版本号`
- **Git tag**：`v4.1.0`

### 变更摘要

本次为 `v4.0.2` 之后的次版本发版，自 v4.0.2 以来累计 5 个提交，主线为新增能力与全模块性能 / 错误边界治理。构建版本号（`CMakeLists.txt` 与 `MODULE.bazel`）同步对齐至 `4.1.0`。

- **utils Process 新增跨平台进程控制接口**：新增 `priority()` / `setPriority()` 进程优先级接口（POSIX nice 值 `[-20,19]`、Windows priority class）与 `cpuAffinity()` / `setCpuAffinity()` CPU 核心亲和性接口（Linux `sched_*affinity`、Windows affinity mask，其他平台显式返回 `Unsupported`）；配套 `ProcessPriorityError` / `ProcessAffinityError` 错误枚举与错误字符串函数，错误经 `std::expected` 显式传播。
- **RingBuffer 模板化支持后端策略选择**：`RingBuffer` 新增 `RingBufferBackendStrategy::{Mmap, Vector, Auto}` 模板参数，默认 `Mmap` 后端提供跨环绕边界单段连续 span / iovec 视图；新增 `RingBuffer::create()` expected 工厂表达容量非法等可恢复失败；Redis / MySQL / RPC / HTTP2 / HTTP / WS / Mongo 等协议客户端同步传播该模板参数。
- **全模块结构体字段重排优化内存布局**：kernel / http / http2 / ws / rpc / mcp / redis / mysql / mongo / etcd / ssl / tracing / utils 及全部 C ABI 模块按访问热点与尺寸重排成员，收敛分散标量 / 指针 / bool 以减少 padding、提升缓存命中率；TCP 完成状态位与部分 kernel IO 上下文状态字段压缩为位域 / `uint64_t`。
- **C++ 对象所有权契约收敛为 move-only + 显式 clone**：覆盖 kernel / utils / HTTP / HTTP2 / WS / RPC / Redis / MySQL / Mongo / Etcd / MCP / SSL / tracing 等模块，非平凡状态对象禁用隐式拷贝、保留显式移动，通过 `clone()` 暴露可审计的深拷贝入口，借用 payload / buffer 视图在 clone 时物化为独立自有存储。
- **错误传播改为 std::expected 显式化并去异常**：SSL 引擎 BIO 接口、SSL 状态机错误映射（`mapSslError` + 编译期 concept 约束，移除 `std::abort()` 兜底）、RESP double 解析（`strtod` 替代会 throw 的 `std::stod`）、RingBuffer 工厂等路径统一改为返回值显式传播错误，移除异常控制流。
- **热路径性能治理**：HTTP/1 与 HTTP/2 静态文件链路异步化（blocking executor + `AsyncWaiter` + server 级共享 cache）；Redis pool 与 RPC 取消通知热路径降低跨线程竞争与取消路径资源放大；HTTP route / writer 与 utils 路由 / 分配优化、LRU 惰性刷新、ConsistentHash 弱内存序；新增 `linux-perf-release` CMake preset 与同机压测口径。
- **修复与覆盖**：修复 Linux io_uring / sendfile 进度回归、epoll AIO 与 file watcher 事件处理边界、etcd HTTP 头 token 误命中与 Linux GCC 14 warning 等问题；补齐 C ABI `*_get_error` 错误字符串入口、ownership surface 测试、SSL BIO 边界测试与 RESP 解析 benchmark 等覆盖。

## v4.2.0 - 2026-07-14

- **版本级别**：次版本（minor）
- **Git 提交消息**：`feat: io_uring 支持 UDP multishot recvmsg 与 epoll 持久读，补齐跨协议竞品基准`
- **Git tag**：`v4.2.0`

### 变更摘要

本次为 `v4.1.0` 之后的次版本发版，自 v4.1.0 以来累计 3 个提交（`a8a6e18`、`add1629`、`4ff9db1`），主线为内核 IO reactor 新能力（io_uring UDP multishot recvmsg、epoll 持久读）与跨协议竞品基准体系，并含 kqueue 稳态优化、RPC 错误边界强化与 Apple libc++ 兼容修复。构建版本号（`CMakeLists.txt` 与 `MODULE.bazel`）同步对齐至 `4.2.0`。

- **io_uring 支持 UDP multishot recvmsg**：`IOUringReactor` 新增基于 `IORING_OP_RECVMSG_MULTISHOT` + provided buffer ring 的 UDP 数据报接收路径，运行时探测内核（≥6.0）与 liburing 能力并回退兼容 one-shot recvmsg；独立 UDP buffer group 与完整数据报 ready 队列按整包交付 payload 与源地址，缓冲不足时丢弃余量保持 UDP 数据报边界。Linux 6.8 绑核 30s 三轮中位 `151,630 pkt/s`，相对 one-shot +5.04%；与 libuv 1.48.0 同口径基本持平（-1.05%，小于自身 CV）。
- **epoll 支持持久 READ 兴趣**：`EpollReactor` 为 recv/readv 引入 `armPersistentRead`，跨 awaitable 持久保留 `EPOLLIN` 并配合注册前非阻塞乐观读消除重复 `epoll_ctl`；WS 固定口径 `epoll_ctl` 由 7,022 降至 36，吞吐 +3.86%。
- **kqueue 稳态注册与多 fd 批量提交优化**：recv/readv 的 `EVFILT_READ` 完成后常驻、send/writev 的 `EVFILT_WRITE` 按需 arm/disarm，简单 awaitable kevent 变更进入 pending batch 统一提交；macOS 32 连接 / 1024B / 5s 中位吞吐 +10.45%。
- **跨协议竞品基准体系**：benchmark 通过 pkg-config 探测并构建 libmysqlclient / hiredis 对照目标；新增 etcd/etcdctl、MySQL/libmysqlclient+mysqlslap、Redis/hiredis+官方 redis-benchmark+连接池自对照、UDP/WS 传输（libuv / libwebsockets）固定口径竞品脚本；新增跨平台 C/C++ 网络竞品对比文档，各模块性能测试文档与原始 CSV/图表/raw 数据归档。
- **RPC 错误边界强化**：RPC 服务器注册与启动改为 `std::expected` 显式错误传播，移除注册所需的 `shared_ptr` 控制块与容器分配；`start()` 语义对齐监听就绪，新增注册表面 / 重复 / 容量耗尽 / bind 失败边界测试与注册压力 benchmark。
- **Apple libc++ 兼容与 kqueue 清理可观测性**：`std::atomic<std::shared_ptr<T>>` 改为普通 `shared_ptr` + 原子自由函数修复 libc++ 构建兼容；kqueue 路径同步处理 `kevent` / `close` 结果并在 remove/close 时丢弃未提交变更。
- **纳入 WS 竞品依赖**：仓库新增 `thirdparty/libwebsockets-4.5.8.tar.gz`（附 SHA-256），供跨平台 WebSocket 明文 echo 竞品基准复现。

## v4.2.1 - 2026-07-15

- **版本级别**：小版本（trivial）
- **Git 提交消息**：`feat: 新增 Debug 构建开关并发布 v4.2.1`
- **Git tag**：`v4.2.1`

### 变更摘要

本次为 `v4.2.0` 之后的小版本发版，新增统一的 Debug 构建开关，并将 fresh 单配置构建的默认类型收敛为 Release。构建版本号（`CMakeLists.txt` 与 `MODULE.bazel`）同步对齐至 `4.2.1`。

- **新增 `GALAY_BUILD_DEBUG`**：开关默认关闭；fresh 单配置构建默认使用 `Release`，开启后强制使用 `Debug`。多配置生成器设置对应默认配置，同时保留 `--config` 的标准显式选择能力。
- **保留非 Debug 显式构建类型**：`RelWithDebInfo`、`MinSizeRel` 等显式配置不被默认 Release 覆盖，现有 Linux 性能 preset 继续使用 `RelWithDebInfo`、`-O2` 与 frame pointer。
- **对齐 CMake presets**：开发类 presets 改用 `GALAY_BUILD_DEBUG=ON`，`linux-perf-release` 显式关闭该开关，避免 Debug 默认值覆盖性能构建配置。
- **补齐配置回归测试**：新增 `config.build_type_option`，覆盖默认 Release、Debug 开关与 `RelWithDebInfo` 保留路径；tracing 配置测试同步使用新开关。全部 7 个 `config.*` CTest 通过。

## v4.3.0 - 2026-07-20

- **版本级别**：次版本（minor）
- **Git 提交消息**：`chore: 发版 v4.3.0 并对齐构建版本号`
- **Git tag**：`v4.3.0`

### 变更摘要

本次为 `v4.2.1` 之后的次版本发版，自 v4.2.1 以来累计 2 个提交（`e8bd857`、`c35cec0`），主线为协程客户端模块的整体重构：awaitable / 池实现下沉 `details/`、`EtcdClusterClient` 重构为无锁 EtcdClient 池租约模型、galay-mcp 命名空间统一为 C++17 形式、HTTP/2 与 RPC 客户端 awaitable 边界统一，并补齐 cluster 连接复用能力与缓存行对齐，附带 h2c 测试就绪竞态修复与 etcd 模块文档更新。构建版本号（`CMakeLists.txt` 与 `MODULE.bazel`）同步对齐至 `4.3.0`。

- **协程客户端 awaitable/池实现下沉 details**：galay-etcd / galay-mysql / galay-redis 三个异步客户端把 awaitable 与池 awaitable 实现迁入 `details/awaitable.{h,inl}` 与 `details/pool_awaitable.{h,inl}`，公开头/源大幅瘦身（mysql `client.cc` 净减约 2000 行），主头只保留接口与类型声明。
- **HTTP/2 与 RPC 客户端 awaitable 边界统一**：`CaptureSchedulerAwaitable`、`H2cUpgradeAwaitable`、`RecvRpcResponseChainAwaitable` 及 RPC 响应读取状态迁入各模块 `details/*_awaitable.{h,inl}`；公开 client 头仅保留前置声明、接口与 details include，并保留任意 `Strategy` / `SocketType` 模板实例化能力。
- **EtcdClusterClient 重构为无锁池**：移除原 cluster wrapper 的 `put/get/del/grantLease/keepAliveOnce/pipeline` 等逐请求方法与内置重试/健康探测循环，改为按 endpoint 预建固定数量 `EtcdClient`、通过 `tryAcquire()` 返回 move-only RAII 租约 `EtcdClientLease` 的无锁空闲队列模型，新增 `acquireConnected()` / `withClient()` 高阶入口；新增 `EtcdErrorType::PoolExhausted`、`EtcdProductionConfig::connections_per_endpoint` 与 `EtcdClusterClientBuilder::connectionsPerEndpoint()`；租约析构或 `release()` 归还连接，错误路径自动归还。
- **galay-mcp 命名空间统一为 C++17 形式**：15 个 mcp 头/源由旧式嵌套 `namespace galay { namespace mcp {` 改为 `namespace galay::mcp {`，不改符号名。
- **池 state 缓存行对齐**：etcd cluster 池状态字段与 redis 连接池 `IdleShard` 按 `alignas(64)` 对齐，降低多线程取还路径的伪共享。
- **cluster 连接复用配置与池便捷 API**：mysql / redis 连接池补齐 `LeaseAwaitable` 等高阶便捷 awaitable。
- **新增 cluster 连接复用 benchmark / 测试 / 示例**：`b6_cluster_connection_reuse`、`t18_cluster_connection_reuse`、`t19_pool_convenience` 与 `e3_client_pool`（import + include 两种形式）覆盖无锁池取还吞吐、池空返回 `PoolExhausted`、租约析构归还、多线程并发安全与池 move 后可用契约。
- **修复 h2c 客户端测试的 listener 启动竞态与错误漏检**：`H2cServer` 新增无锁 `isReady()`，仅在 listener 完成 bind/listen 后报告就绪；`http2.h2ccl` 改为有界等待可观测 ready 状态，并完整处理 connect/shutdown 的 `std::expected` 错误。
- **更新 etcd 模块文档**：同步架构设计 / API 参考 / 使用指南 / 示例 / 性能测试 / 常见问题，新增 `docs/cpp/modules/etcd/refactor-plan.md` 重构执行文档。

## v4.4.0 - 2026-07-28

- **版本级别**：次版本（minor）
- **Git 提交消息**：`feat: 重构类型安全 CLI 解析接口并发布 v4.4.0`
- **Git tag**：`v4.4.0`

### 变更摘要

本次为 `v4.3.0` 之后的次版本发版，主线为重构 galay-utils 命令行解析能力：以编译期类型安全的 `Opt<T>` / `Positional<T>` 替换旧 `Arg` / `ArgValue` 接口，新增完整的选项、位置参数、变量绑定、多级子命令和帮助输出语义，并将错误统一为 `std::expected<void, CliError>` 显式传播。该公开 API 替换对直接使用旧 CLI 接口的调用方不兼容；由于未获得升级主版本的明确授权，本次按新增功能保守发布为 `v4.4.0`。构建版本号（`CMakeLists.txt` 与 `MODULE.bazel`）同步对齐至 `4.4.0`。

- **类型安全 CLI API**：新增 `opt<T>()`、`flag()`、`pos<T>()`、`sub()` 与 `on()` 声明入口，支持默认值、必选参数、外部变量绑定、重复参数、候选集合、命名位置参数和多级子命令；解析结果可直接通过 `value()` / `values()` 获取。
- **无异常错误传播**：数值转换使用 `std::from_chars` 或无异常浮点回退，解析失败通过 `CliErrorCode` / `CliError` 与 `std::expected` 返回；帮助和版本请求作为正常终止状态处理。
- **现代命令行语义**：支持 `--opt=value`、`-o value`、`-ovalue`、短选项合并、`--no-flag`、`--` 终止符、重复解析状态重置，以及父子命令必选校验和帮助优先级。
- **公开头边界重组**：将 CLI 实现拆分为 `app/{error,value,arg,positional,cmd,app}.hpp`，聚合入口与 C++ module prelude 同步更新；旧 `ArgType`、`ArgValue`、`Arg`、`addArg()`、`getAs()` 等接口不再保留。
- **测试与文档同步**：扩展 `utils.app_cli_config` 覆盖成功路径、无效值、必选参数、候选集合、位置参数、子命令、帮助/版本和重复解析边界；更新 utils API 参考与使用指南。全新 Release 构建下 `utils` 标签 19/19 通过。

## v4.4.1 - 2026-07-28

- **版本级别**：小版本（trivial）
- **Git 提交消息**：`refactor: 收敛 App 命令行执行入口并发布 v4.4.1`
- **Git tag**：`v4.4.1`

### 变更摘要

本次为 `v4.4.0` 之后的小版本发版，主线为收敛 galay-utils 的 `App` 公开执行入口：删除缺少实际使用场景的 `parseArgs()` 仅解析接口，统一由 `run()` 完成参数解析、帮助/版本处理、错误输出和命令回调执行。解析失败仍在内部通过 `std::expected<void, CliError>` 显式传播，再由 `run()` 转换为进程退出码。该公开 API 删除对直接调用 `parseArgs()` 的源码不兼容；版本级别按用户明确要求保持为小版本。构建版本号（`CMakeLists.txt` 与 `MODULE.bazel`）同步对齐至 `4.4.1`。

- **收敛 `App` 公开 API**：删除 `App::parseArgs()` 及其公开文档，保留 `run()` 作为唯一命令行生命周期入口。
- **测试与文档对齐**：移除 parse-only 专用测试，module import smoke 改为调用 `run()`；utils API 参考与使用指南不再展示已删除入口。
- **验证结果**：重新配置并构建 utils 测试，`utils` 标签 19/19 通过，`git diff --check` 通过。

## v4.4.2 - 2026-07-29

- **版本级别**：小版本（trivial，用户指定）
- **Git 提交消息**：`feat: 扩展 App 版本短选项与空参数帮助并发布 v4.4.2`
- **Git tag**：`v4.4.2`

### 变更摘要

本次为 `v4.4.1` 之后的小版本发版，主线为补齐 galay-utils `App` 的版本短选项与空参数帮助语义。`version()` 现在复用普通 flag 注册路径，可显式声明 `-v` 等短名；无命令行实参时 `run()` 直接输出 Usage。配套补齐 CLI 压力基准，并修正 RingBuffer 测试中已经落后于 `std::expected` 错误契约和 mmap 降级语义的断言。构建版本号（`CMakeLists.txt` 与 `MODULE.bazel`）同步对齐至 `4.4.2`。

- **版本短选项复用普通 option 注册**：`App::version(text, shortName)` 内部通过 `flag("version", shortName, ...)` 注册真实选项，短名、长名、帮助展示和查找统一走 `Cmd` 既有流程；`version("1.2.3", 'v')` 同时支持 `-v` / `--version`，默认 `shortName='\0'` 保持既有仅长选项行为。
- **空参数直接输出 Usage**：顶层 `App::run()` 在 `argc <= 1` 时打印帮助并返回 0，避免命令无参数时静默结束或先报告必选参数缺失。
- **CLI 边界测试与压力覆盖**：`utils.app_cli_config` 新增版本短名、帮助标签和空参数 Usage 断言；新增 `benchmark_utils_app_cli_dispatch_pressure`，10 万次压力下空参数帮助约 57 万次/秒、版本短选项约 187 万次/秒。
- **RingBuffer 测试对齐显式错误传播**：零容量边界改为检查 `RingBuffer::create(0)` 返回 `RingBufferError::kInvalidCapacity`，移除旧 `try/catch`；默认后端 iovec 断言允许 mmap 创建失败后合法降级为 vector，并校验全部片段总长度。
- **验证结果**：`utils.app_cli_config` 通过，`utils.buffer_queue_ring` 连续运行 10 次通过，CLI 压力基准通过，`git diff --check` 通过。当前受限 macOS 环境的 utils 全量测试仍有 3 项既有环境/专项假设失败：进程优先级设置权限，以及两个 mmap 专项用例在 mmap 创建失败并降级 vector 后仍要求单 iovec；均不由本次改动引入。

## v4.5.0 - 2026-08-07

- **版本级别**：次版本（minor，用户指定保持 v4）
- **Git 提交消息**：`feat: 重构 C API 并补齐 Channel family`
- **Git tag**：`v4.5.0`

### 变更摘要

本次为 `v4.4.1` 之后的次版本发布，自 v4.4.1 以来累计 28 个提交，主线是重构 galay-kernel 高性能并发通道体系，新增并优化 MPMC、MPSC 与 SPSC 有界/无界通道，并补齐 C ABI、协程超时、关闭排空和跨语言性能验证。仓库此前已记录 `v4.4.2` 版本元数据与发布说明，但未创建对应远端 tag；本次 `v4.5.0` 覆盖 `v4.4.1..HEAD` 的全部累计变更。构建版本号（`CMakeLists.txt` 与 `MODULE.bazel`）同步对齐至 `4.5.0`。

- **MPMC 通道体系**：新增基于固定容量 Vyukov ring 的有界通道和可回收分段结构的无界通道，提供同步、协程、批量、超时与 close/drain 语义；退役 block 逆序扫描、token 局部缓存和 waiter 冷路径进一步降低稳态开销。
- **MPSC 专用通道**：新增有界/无界 MPSC 实现，支持 producer token、无分配批量排空和每 producer 独占 SPSC ring 模式；修复换块、waiter/close 与失败发送边界，回退尚未成熟的统一工厂 API。
- **SPSC 数据面**：新增运行时与编译期容量 ring、分块复用无界队列及有界/无界异步通道；通过 endpoint split、本地 cursor、对端 cursor 缓存与 Linux 非对称内存屏障优化批处理和稳态轮询热路径。
- **C Channel family ABI 与公开边界**：C API 默认启用，移除旧平面 `bounded` / `mpsc` / `unsafe` channel 入口；按 `mpmc` / `mpsc` / `spsc` 拓扑和有界/无界类型提供完整 ABI，统一错误码、单条/批量收发、C coroutine 超时、关闭、容量状态与安装头验证。无界 MPMC/MPSC 提供线程专属 producer token，MPMC 另提供 consumer token；Linux 24 字节消息 MPMC 4P4C token 路径中位吞吐约 53.43M msg/s，较默认路径约 9.88M msg/s 提升约 5.2 倍。
- **异步 I/O 与调度边界**：C/C++ 异步 I/O 文件和公开类型统一为 `async_*` / `Async*`，异步同步原语归入 async 模块；waker 恢复改用 owner scheduler 无分配入口，并修复 HTTP/2 晚到事件导致的生命周期问题。
- **测试与性能证据**：补齐 MPMC/MPSC/SPSC 对照 Rust Crossbeam 的 paired runner、bootstrap 95% 置信区间、FIFO/checksum/drain 门禁和原始样本；新增部署与腾讯 Linux/NUMA/性能分析脚本，并停止跟踪本机性能产物。
- **累计修复**：修复 io_uring connect、HTTP/2 registration 稳定性、channel timeout 完成竞争、SPSC 跨块回收、MPSC waiter/close、Mongo topology、MySQL RSA OAEP 与 GCC 14 诊断等问题，并系统处理测试、benchmark 和清理路径中的非 `void` 返回值；TCP C API 的 socket 创建失败改为返回 I/O 错误，socket 权限受限时仅跳过依赖本地 socket 的 CTest。
