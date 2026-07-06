# Changelog

本项目所有显著变更均会记录在本文件中。

## 如何维护

- **版本规则**：遵循 `major.minor.patch`。大改动（架构/目录重组/核心接口变更）升 `major`；新增功能升 `minor`；修复 bug、文档、配置、chore 等小修补升 `patch`。
- **更新时机**：每次提交前都必须更新本文件。未发版的变更写入 `## [Unreleased]` 节；发版时把 `Unreleased` 收束为新的版本节，并在最上方补一个空的 `## [Unreleased]`。
- **标题格式**：`## [vX.Y.Z] - YYYY-MM-DD`。
- **内容粒度**：按 `Added` / `Changed` / `Removed` / `Fixed` / `Docs` / `Chore` 等小节归纳，只记录最重要的变更，不逐行抄写 diff。

## [Unreleased]

### Changed

- **全模块结构体字段重排以优化内存布局**：对 kernel / http / http2 / ws / rpc / mcp / redis / mysql / mongo / etcd / ssl / tracing / utils 及全部 C ABI 模块（`src/c/galay-*-c`）中的 struct / class 成员按访问热点与尺寸重排，把分散的小尺寸标量、指针与 bool 收敛到更紧凑的位置以减少 padding、提升缓存命中率；同步更新聚合初始化 `{...}` 顺序与构造初始化列表顺序以匹配新声明顺序（消除 `-Wreorder`）。
- **TCP 完成状态位压缩为位域**：`c_coro_tcp_bridge.cc` 的 `CoroTcpOperationBase` 用单个 `uint64_t m_flags` 位标记替代 `m_finished` / `m_complete_accepted` 两个独立 bool，并提供 `finished()` / `completeAccepted()` / `setFinished()` / `setCompleteAccepted()` 访问器，进一步压缩对象尺寸。
- kernel IO 上下文中部分 bool / 枚举状态字段统一改为 `uint64_t` 承载：`ReadvIOContext` / `WritevIOContext` 的 `m_immediate_result`、`FileWatchIOContext` 的 `m_events`、`SequenceAwaitableBase` 的 `m_registered`。

### Added

- **utils 新增跨平台进程优先级接口**：`Process` 新增 `priority()` / `setPriority()` 静态方法，POSIX 平台基于 `getpriority` / `setpriority`（nice 值 `[-20,19]`），Windows 平台映射到 priority class；新增 `ProcessPriorityError` 错误枚举与配套的 `processPriorityErrorString()` 错误描述函数，错误经 `std::expected<T, ProcessPriorityError>` 显式传播，errno / `GetLastError` 立即转换为具体错误码（遵循错误显式传播与每个错误码配套错误字符串的约定）。`module_prelude.hpp` 补齐 `<cerrno>` / `<sys/resource.h>` 头，并新增 `test/cpp/utils/t11_platform_process_system.cc` 白盒测试。

## [v4.0.2] - 2026-07-06

### Docs

- 补全 C ABI 模块文档：在 `docs/c/modules/` 下新增 `bridge` / `common` / `utils` 共享层 README，给出源码位置、CMake target / alias、依赖与主要职责，与既有各协议模块文档对齐。
- 更新 `README.md`：模块化构建说明拆分为 C++ 模块（`src/cpp/galay-*`）与 C ABI 模块（`src/c/galay-*-c`），目录树同步区分 `docs/cpp/modules/` 与 `docs/c/modules/`，并补上 C ABI 文档入口指引。

### Removed

- 删除过期文档 `docs/benchmark_plan.md`：其内容已被 v4.0.1 各模块 `05-性能测试.md` 与归档基准数据取代。

## [v4.0.1] - 2026-07-05

### Added

- 新增 galay-framework 开发 skill（`agent/skill/galay-usage/`），作为在 galay 上开发服务端 / 客户端 / 中间件与 C/FFI 的统一入口：`SKILL.md` 给出 Runtime / `Task<T>` / `std::expected` 错误传播心智模型、include 前缀与命名空间、CMake 链接 target、构建开关与平台后端（io_uring / epoll / kqueue）速查及 13 个 C++ 模块 + C ABI 模块地图；`references/cpp-api.md` 汇总 13 个 C++ 模块公开类型与方法签名；`references/c-api.md` 覆盖 C ABI 错误约定、runtime / coro 驱动模型、每模块 handle 与完整 C 示例。
- **全模块新增 TCP_NODELAY 可配置化**：为 etcd / http / http2 / mcp / mysql / redis / rpc / ws 各模块的客户端与服务端配置新增 `tcp_no_delay` 字段（默认开启）与对应 `tcpNoDelay()` builder 方法；连接建立或 accept 后按配置对 socket 启用 `TCP_NODELAY`，选项失败按各模块语义显式传播错误（客户端连接路径回滚并返回失败，服务端 accept 路径记录 WARN 后继续）。新增各模块 `t*_nodelay_config` 白盒测试，通过 `getsockopt` 验证 socket 选项随配置在默认开启与显式关闭两种情况下均正确生效。
- **WS benchmark 支持 TCP_NODELAY 开关与目标 URL 参数**：新增 `benchmark/cpp/ws/ws_benchmark_args.h` 集中解析命令行参数；`b5_ws_server_throughput` 第 3 个、`b7_wss_server_throughput` 第 5 个业务参数控制 server 端 `TCP_NODELAY`（默认开启，传 `off`/`false`/`0` 关闭）；`b6_ws_client_throughput` 第 4 个业务参数指定目标 URL，默认仍为 `ws://127.0.0.1:8080/ws`，便于在 8080 被占用时改用其他端口。
- 新增 `scripts/common/500_install_skill.sh`：把指定 skill 目录以同名方式安装到目标目录，供本地或代理环境复用 galay skill。
- **新增 socket 局部 SIGPIPE 抑制选项**：`HandleOption::handleNoSigPipe()` 在 macOS/BSD 等支持平台上启用 `SO_NOSIGPIPE`，并在 `TcpSocket::openHandle`、`handleAccept` 创建或接收 socket 时默认启用，使写入断连连接返回 `EPIPE` 而非投递 SIGPIPE；不支持该选项的平台为空操作。
- 新增 `test/cpp/kernel/t139_sigpipe_policy.cc`：验证 `Runtime` 构造不修改全局 SIGPIPE 处置、Linux `handleSend` / `handleWritev` 写路径走 `MSG_NOSIGNAL`、支持平台 `TcpSocket::create` 默认启用 `SO_NOSIGPIPE`。
- **全模块性能测试文档与基准数据落地**：为 `kernel` / `http` / `http2` / `ws` / `rpc` / `ssl` / `tracing` / `utils` / `redis` / `mysql` / `mongo` / `etcd` 各模块补全或新建 `05-性能测试.md`，记录可复现的 benchmark target、运行命令与同环境性能对比快照；并在各模块下新增 `benchmark_data/`、`configs/` 归档压测结果（CSV / TXT / SVG / log）与复现配置。
- 新增 `docs/benchmark_plan.md`（全模块性能压测计划：控制变量、指标、环境与分模块场景）与 `docs/machine_config.md`（测试机器硬件与系统配置记录）。
- `scripts/http2/300_http2_h2load_compare.sh` 新增 `--post-echo-best` / `--post-echo-matrix` best-of 矩阵模式：遍历 server 线程数、最大流数与 h2load 线程 / 客户端 / 流数组合，输出 galay 与 nghttpd 的最佳吞吐与配置对比，默认要求 Release 构建并支持 `build-release` 路径回退。
- 新增 `test/scripts/t2_http2_h2load_compare.py`，并在 `test/scripts/CMakeLists.txt` 注册 `scripts.http2_h2load_compare` CTest 用例。
- 新增 `benchmark/cpp/http/b17_static_server_throughput.cc`：HTTP/1.1 静态文件 echo 压测程序，每请求执行 stat+open+read+close 真实磁盘读取，镜像 Apache httpd 静态服务工作量，作为 galay-static vs httpd-static 的公平对比基线（区别于 b1 的内存固定响应）。

### Changed

- **统一 CMake 安装包为单一 `galay` 包**：移除顶层 `CMakeLists.txt` 中按模块生成 `galay-kernel` / `galay-utils` / `galay-http` 等独立 package config 的 foreach，并删除模板 `cmake/galay-module-config.cmake.in`；安装后只在 `lib/cmake/galay` 下导出 `galayConfig.cmake` / `galayConfigVersion.cmake` / `galayTargets.cmake`，外部项目统一通过 `find_package(galay CONFIG REQUIRED)` 后按需链接 `galay::<module>`。同步收紧 install 布局校验（`test/cpp/config/install_include_layout.cmake`、`test/cpp/mysql/package/CMakeLists.txt.in` 与 `package_consumer_smoke.cmake`、`scripts/common/103_verify_module_layout_install_bazel.sh`）强制断言不再安装按模块的 package 目录，`test/cpp/kernel/t94_alignsrc.cc` 改为校验旧模板已移除；并刷新全模块快速开始 / API 参考 / 常见问题文档与 `agent/skill/galay-usage/SKILL.md` 的引入方式（含 CMake target 与 Bazel label 映射表）。
- **重组 `scripts/` 目录**：将原本散落在 `scripts/` 根下的验证与基准脚本按模块迁入 `common/`、`etcd/`、`http2/`、`mongo/`、`mysql/`、`redis/`、`rpc/` 子目录，并统一加 `1xx`/`2xx`/`3xx`/`4xx`/`5xx` 数字前缀，以稳定执行顺序并按域归类。
- **重组 `agent/skill/` 目录**：将顶层 `SKILL.md` 与 `references/` 迁入 `agent/skill/galay-usage/` 子目录，使 skill 以命名目录形式承载，便于安装与复用。
- **构建开关默认收敛**：`cmake/option.cmake` 将 `BUILD_TESTING`、`GALAY_BUILD_EXAMPLES`、`GALAY_BUILD_BENCHMARKS` 默认值由 `ON` 改为 `OFF`，使测试 / 示例 / 基准目标改为按需开启，避免默认全量构建。
- **kernel 写路径改用局部 SIGPIPE 抑制**：`handleWritev` 由 `writev()` 改为 `sendmsg()` 并携带 `MSG_NOSIGNAL`，与 `handleSend` / `handleSendTo` 对齐；框架不再依赖全局 SIGPIPE 处置，向断连 socket 写入时返回 `EPIPE` 而非投递 SIGPIPE。
- **socket 选项失败的清理路径不再静默 `close`**：`TcpSocket::openHandle` 与 `handleAccept` 在 `handleNoSigPipe` / IPv6-only 等选项失败时检查 `close()` 返回值，失败时显式上报 `kDisconnectError`，避免错误被吞掉。
- 构建版本号对齐 git tag：`CMakeLists.txt` 的 `project(galay VERSION ...)` 与 `MODULE.bazel` 的 `module(... version = ...)` 自 `4.0.0` 升至 `4.0.1`。

### Fixed

- 修复 HTTP server 路由参数丢失：`HttpServer` 在通过 `m_router->findHandler(method, uri)` 匹配路由后，未将解析出的路径参数回填到 `request`，导致业务 handler 始终拿不到 `/users/:id` 这类路径参数；现已在路由命中后立即 `request.setRouteParams(std::move(params))`，使 handler 能正确读取路由参数。

### Removed

- 清理被新 skill 取代的过期文档：`docs/c-abi-encapsulation-optimization.md`、`docs/cpp-modules-optimization.md`、`docs/naming-and-cmake-optimization.md`、`docs/rust-ffi-zero-overhead-guide.md`、`docs/文档审查报告.md`、`docs/README.md`，以及仅停留在 v3.0.0 的旧 `docs/release_note.md`（自本次起按发版节重建）。

### Docs

- 刷新 `docs/cpp/modules/ws/05-性能测试.md`：补做 WS echo 同类竞品实测对比（`gorilla/websocket v1.5.3`，1000 连接 / 30s / 1KB payload），归档原始日志（`benchmark_data/raw/`）、竞品 fixture（`gorilla_echo_server_2026_07_04/`）与汇总 CSV，并补充 server `TCP_NODELAY` 开关与 client 目标 URL 参数用法。
- 同步更新 `CHANGELOG.md` 历史版本节、`benchmark/cpp/rpc/README.md`、`docs/cpp/modules/http2/05-性能测试.md`、`docs/cpp/modules/rpc/performance-comparison.md` 与 `test/cpp/mysql/t12_auth_plugins.cc` 中引用的脚本路径，指向重组后的新位置。
- 刷新 HTTP / HTTP2 性能测试报告与基准数据：更新 `docs/cpp/modules/http/05-性能测试.md`、`docs/cpp/modules/http2/05-性能测试.md` 及对应 `benchmark_data/`（http11 同语言对比、h2load post-echo 对比 CSV/TXT、post-echo 吞吐与延迟 SVG 图表）。

## [v4.0.0] - 2026-07-02

### Added

- 新增 C ABI 封装约定落地：`C_IOResultCode` 诊断字符串和 `galay_status_t` 映射 helper，补齐 EOF/Timeout/Cancelled 通用状态码，并新增 `galay_iovec_t` 作为 public C ABI scatter/gather buffer 类型。
- 新增 Linux examples/benchmarks 全量执行矩阵脚本 `scripts/common/105_verify_linux_exec_matrix.py`，支持按 build root 扫描可执行文件、区分 PASS/SKIP/LONG_RUNNING/EXTERNAL_DEP/NEEDS_PEER/FAIL/MISSING，并对已知 C/S 架构测试按先启动 server、再运行 client、最后清理 server 的顺序验证。
- 新增 `consistent_hash.hpp` 无阻塞锁源码边界测试与 lookup benchmark，锁定一致性哈希实现不再引入 `std::mutex` / `std::shared_mutex` 等会阻塞协程调度线程的同步原语。
- 新增 Redis C standalone direct coroutine async client 最小闭环：`galay_redis_client_connect`、`galay_redis_client_command_async` 与 `galay_redis_client_close`，通过本地 mock Redis loopback 覆盖 PING/PONG，并补齐对应 C test、example 与 smoke benchmark。
- 新增 Redis C async `AUTH`、`SELECT` 与 pipeline API：支持 pipeline 命令缓存、批量 reply 保留和统一释放，并补齐本地 mock loopback test、example 与 smoke benchmark。
- 新增 MySQL C direct coroutine async client 最小闭环：`galay_mysql_client_connect_async`、`galay_mysql_client_query_async` 与 `galay_mysql_client_close_async`，通过本地 mock MySQL packet loopback 覆盖 handshake、COM_QUERY 和 result packet，并补齐对应 C test、example 与 smoke benchmark。
- 新增非 kernel C module target Phase 1 基线：按 `src/c/galay-<module>-c` 目录组织补齐 `galay-c-common`、`galay-c-utils`、`galay-c-ssl`、`galay-c-http`、`galay-c-ws`、`galay-c-http2`、`galay-c-redis`、`galay-c-rpc`、`galay-c-mysql`、`galay-c-mongo`、`galay-c-etcd`、`galay-c-mcp`、`galay-c-tracing` 的 CMake target、纯 C public header、最小 wrapper implementation 与 CTest surface 注册。
- 新增 `galay-c-bridge` 内部 C/C++ bridge 模块，将 C coroutine 旁路 bridge 从 C++ kernel core 拆到 `src/c/galay-bridge-c/coro-c`，并由 `galay-c-kernel` 显式依赖。
- 新增 HTTP/2 production hardening 覆盖：补齐 SETTINGS 校验、h2c HTTP2-Settings 解码、peer/local settings 应用、HEADERS/CONTINUATION 与 DATA outbound limit 测试，覆盖 `MAX_FRAME_SIZE`、`MAX_HEADER_LIST_SIZE`、ACK payload、非 0 stream SETTINGS 和 decoder header-list limit。
- 新增 C async API 补齐计划文档 `docs/c/modules/async_api_completion_plan.md`，明确当前非 kernel C target/async ABI 缺口，并按 HTTP/WS/HTTP2、Redis/MySQL/Mongo、Etcd/MCP/RPC、SSL/tracing 分阶段实现。
- 新增 CMake 守卫 `config.kernel_internal_includes_relative`：递归扫描 `src/cpp/galay-kernel` 全部 `.cc/.h/.hpp/.inl` 源码，禁止内部实现通过 `galay/cpp/galay-kernel/` 公共 include 前缀引用自身头文件，强制改用相对路径。
- 新增 C Kernel TCP/UDP/AsyncFile/FileWatcher timeout C ABI：补齐 connect/accept/accept_loop/recv/recv_loop/send/send_loop/close、recvfrom/sendto loop、AsyncFile read/write/close 与 FileWatcher watch 的毫秒级 timeout API，并新增对应 C 回归测试、timeout 示例、timeout smoke 和混合 API pressure benchmark。
- 新增 HTTP/1.1 route-mode 生产策略与边界测试，覆盖请求头/URI/body 限制、Content-Length/Transfer-Encoding 冲突校验、keep-alive 生命周期、请求/响应超时和 sendfile 写超时路径。
- 新增 C Kernel UDP 双进程 client/server 互压 benchmark：`benchmark_c_kernel_udp_socket_server_throughput` 与 `benchmark_c_kernel_udp_socket_client_throughput` 对齐 C++ UDP server/client 压测口径，支持独立 server/client 进程、显式端口、并发 client、消息数、payload、duration 与 IO scheduler 参数。
- 新增 C Kernel async/concurrency C ABI wrapper：补齐 UDP socket、AsyncFile、AioFile、FileWatcher、AsyncMutex、AsyncWaiter、MpscChannel、UnsafeChannel 的 `.h/.cc`、回归测试、示例与 benchmark smoke，并接入 `galay-c-kernel` 构建和 C Kernel 文档。
- 新增 C Kernel direct coroutine async C ABI 覆盖：UDP、AsyncFile、FileWatcher、AsyncMutex、AsyncWaiter、MpscChannel、UnsafeChannel 通过旁路 C coroutine bridge 复刻 C++ async 能力，并新增 TCP iov/sendfile benchmark 覆盖。
- 新增 C Kernel `TcpSocket` callback API，补齐 `connect` / `accept` / `recv` / `send` / `close`，并新增 `accept_loop` / `recv_loop` / `send_loop`，loop callback 可通过返回值控制是否继续。
- 新增 C Kernel `TcpSocket` 回归、示例与 benchmark：覆盖 async callback、close 集成、loop callback、echo 示例、生命周期压测，以及双进程 TCP echo QPS/吞吐压测。
- 新增 C++ 模块审计修复的边界测试与源码守卫，覆盖 kernel task/timeout/iov/resource、HTTP/WS/HTTP2 协议边界、Redis/MySQL/Mongo/etcd 客户端边界、MCP/SSL/tracing 安全生命周期，以及 utils umbrella/resource 错误边界。
- 新增对应压力/性能基准，覆盖 kernel task timeout/resource error、HTTP/WS 协议边界、Mongo expected 错误传播、utils resource error，以及 RPC payload scaling 等场景。
- 新增 C API no-exception 源码边界守卫，覆盖 MySQL/Mongo/tracing/HTTP2/Redis/etcd C 包装层与 Base64 工具，防止异常控制流重新进入这些边界文件。
- 新增 Redis 连接池 waiter 状态仲裁测试、黑盒等待者测试、Rediss acquire 连接态回归测试与压力基准，覆盖 release/timeout/cancel 竞争和等待者统计泄漏。
- 新增 C ABI TCP accept callback 边界测试、示例与 smoke 基准，覆盖持续 accept loop 通过回调交付 accepted socket 与 peer 信息。
- 新增 `galay-kernel-c` 分层 C ABI：`core-c/runtime_c.*` 暴露 runtime 生命周期接口，`common-c/host.h` 定义 `C_Host` 值类型，`async-c/tcp_socket_c.*` 暴露 TCP socket 句柄、bind 和 accept callback 相关声明。
- 新增 MCP 命名边界测试，递归扫描 `src/cpp/galay-mcp` 中的 C++ 源码符号，防止 MCP 自有函数/方法重新出现大写开头驼峰命名。
- 新增 kernel sequence 错误边界、MySQL packet 边界、RPC core/etcd adapter 表面、tracing shutdown timeout 等回归测试与压力基准。
- **C ABI 符号可见性与版本接口**：`src/c/CMakeLists.txt` 新增 `galay_configure_c_api_target`，为 14 个 `galay-c-*` 共享库 target 统一配置 `GALAY_C_SHARED` / `GALAY_C_BUILDING` 宏、hidden visibility 与 `VERSION` / `SOVERSION` 属性；`galay_c_defs.h` 新增 GCC/Clang 下 `visibility("default")` 导出属性；`galay_c_error.h/cc` 新增 `galay_c_version_{major,minor,patch}()` 与 `GALAY_BUFFER_TOO_SMALL` 状态码，并新增 `test/c/common/abi_version_smoke.c` 锁定编译期宏与运行期函数一致。

### Changed

- C coroutine bridge 入口从裸 `void*` 收敛为 `GalayCore*` 具名 opaque C 类型，TCP readv/writev public ABI 改用 `galay_iovec_t` 并在实现内部转换为平台 `struct iovec`。
- 完成 `docs/cpp-modules-optimization.md` 中一轮 C++ 模块低风险优化落地：覆盖 utils Base64、SSL/WS/HTTP/HTTP2 hot path、Redis/MySQL/Mongo/etcd/MCP 协议解析、tracing traceparent/sampler、kernel timer drain 与 RPC pool endpoint key，并补齐对应 CTest 与 benchmark 覆盖；跨行为连接池真复用、Mongo TLS URI、etcd HTTPS 等高风险项保留为单独设计任务。
- `ConsistentHash` 从 `std::shared_mutex` 读写锁改为 copy-on-write 原子快照发布与 reader-count retired snapshot 回收，读路径只做原子快照加载和原子节点状态更新，避免 coroutine 调度线程被阻塞锁卡住。
- Linux examples/benchmarks smoke 验证统一以短 workload 运行重型 benchmark，并将 C stackful coroutine 边界 CTest 配置为串行运行，降低 4 核 Linux 主机上并发 CTest 对短 join/cancel 窗口的干扰。
- 优化 HTTP/2 HPACK 动态表查找热路径，按 ring 顺序直接扫描动态表以减少重复边界检查和取模；HTTP client 与 header parser benchmark 补充 P50/P90/P95/P99 等尾延迟观测输出；`TimingWheelTimerManager` 级联复用同一次 tick 的时间戳并修正默认 tick 注释。
- HTTP/2 server/client/h2c 路径移除异常兜底，错误通过返回值、GOAWAY/RST_STREAM 或日志可观测路径传播；HTTP close 清理路径改为 inline 处理 close 返回值，避免 coroutine close helper 过度拆分。
- C Kernel TCP async C ABI 破坏式迁移为 direct C coroutine 形态：`tcp_socket_c` 直接提供 `C_IOResult` 返回的 accept/connect/recv/send/close 接口，移除 runtime callback/spawn 桥接路径，并同步更新 TCP C 测试、示例和 benchmark。
- HTTP/1.1 route-mode 接入 `HttpServerPolicy`，将 reader/writer timeout、request limits、keep-alive idle timeout 和 response write timeout 统一由 server/router 策略驱动。
- C Kernel async/concurrency C ABI 统一迁移到 direct coroutine 形态：测试、示例和 benchmark 不再依赖 callback/spawn wrapper，改为在 C coroutine 内直接调用 C async API 并显式处理返回值。
- C 栈式协程 context 支持矩阵改为显式诊断：Linux/aarch64 当前不声明支持，CMake 会输出不支持原因，并让 direct C coroutine 测试、示例和 benchmark 带 skip reason 跳过。
- galay-kernel 内部源码 include 由公共前缀 `<galay/cpp/galay-kernel/...>` 统一改为相对路径（同目录直引、跨目录用 `../core/`、`../common/`），覆盖 async/core/common 下的 reactor、scheduler、socket、file、logger 等实现文件，避免内部实现依赖安装态公共 include 前缀。
- C++ 模块文档目录从 `docs/modules` 收敛到 `docs/cpp/modules`，顶层 README、`.gitignore` 与模块文档导航同步改向新的 cpp 文档路径。
- C/C++ 示例、测试和 benchmark 的 CMake 注册方式批量改为 `file(GLOB ... CONFIGURE_DEPENDS)`，减少新增源文件时的手工 target 维护。
- RPC C++23 module file set 改为直接注册到 `galay-rpc` / `galay::rpc`，保持模块入口文件与 CMake source list 规则一致。
- C ABI 非 kernel 模块公开头与实现文件统一从 `<module>.h` / `<module>.cc` 重命名为 `<module>_c.h` / `<module>_c.cc`，kernel C ABI 新增 `kernel_c.h` 伞形公开头，并同步更新测试、示例、benchmark 与文档 include 路径。
- 调整 C Kernel `TcpSocket` accept/recv/send 结果结构：accepted socket 直接随 `galay_kernel_tcp_accept_result_t` 返回，移除 `has_socket` 与 `take_socket`；recv/send 结果补充原始 buffer 与 length，便于 callback 链式处理。
- C Kernel TCP/Host C enum 成员统一改为带前缀命名：`C_TcpSocket*` 与 `C_IPType*`，移除旧的无前缀 `Success` / `ParameterInvalid` / `IPV4` / `IPV6` 等枚举名，并同步更新测试、示例与 benchmark。
- C Kernel 测试、示例和 benchmark CMake 改为 `file(GLOB ... CONFIGURE_DEPENDS)` 自动收集源文件，避免新增用例时逐个登记。
- Mongo BSON/ObjectId/OP_MSG 编码边界改为 `std::expected` 显式传播错误；非法 ObjectId、BSON key 与 OP_MSG 编码失败不再通过异常逃逸，客户端边界统一转换为 `MongoError`。
- C++23 `.cppm` 安装策略改为保守模式：普通 header install 不再安装未验证 module facade，后续只允许具体 `CXX_MODULES FILE_SET` module target 安装自己的模块接口文件。
- 多模块协议与资源路径补齐显式边界处理，包括 HTTP/WS/HTTP2/RPC framing、Redis pool wait/RESP limit、MySQL packet length、MCP transport limit、SSL init/hostname/OAEP 与 tracing shutdown/escaping。
- Base64 解码改为显式可解码检查入口，C API、Mongo 与 etcd 调用侧先判定输入合法性，再用返回值表达错误，避免依赖异常作为错误通道。
- MCP 自有 JSON 文档、写入器、解析辅助函数及相关调用点统一改为小写开头驼峰命名，保留类型名、构造函数、协议字段和 JSON-RPC 方法字符串不变。
- RPC 的 etcd adapter 改为由 `GALAY_RPC_ENABLE_ETCD` 控制并编译进 `galay::rpc`，不再导出单独的 `galay::rpc-etcd` 目标。
- C++23 module 示例、测试与文档统一改为链接 `galay::<module>` canonical target，module file set 直接挂载在 `galay-<module>` 上，不再使用独立 `galay-<module>-modules` facade target。
- **统一 utils C ABI 状态码到 `galay_status_t`**：`galay_utils_status_t` 改为 `galay_status_t` 别名，`GALAY_UTILS_*` 宏映射到 `GALAY_*`；utils 全部导出函数签名统一返回 `galay_status_t` 并标注 `GALAY_C_API`，`utils_c.h` 改用 `GALAY_C_BEGIN_DECLS`；`test/c/utils/header_smoke.c` 增加 `_Static_assert` 锁定新签名，`galay-c-utils` 显式链接 `galay::c-common`。
- kernel C API 实现按 runtime、TCP socket 拆分到 `core-c` 与 `async-c`，并将 runtime / TCP socket C 句柄调整为 FFI 可见的 `void*` 载荷结构。
- kernel common 负载均衡头文件从 `async_strategy.hpp` 更名为 `balancer.hpp`，同步更新 RPC discovery include，避免旧文件名残留。

### Removed

- 移除独立的 `tcp_socket_coro_c.{h,cc}` direct coroutine TCP C API 文件，相关能力并入 `tcp_socket_c.{h,cc}`。
- 移除旧 `docs/modules` 文档树，当前 C++ 模块文档统一从 `docs/cpp/modules` 进入。
- 移除旧 C 跨模块 smoke 示例与 benchmark 目录，只保留当前已落地的 C Kernel TcpSocket 文档、测试、示例和压测资产。
- 移除 C ABI TCP accept 单次 awaitable handle 接口 `galay_kernel_tcp_accept_{start,wait,join,cancel,destroy}`，改用 `galay_kernel_tcp_socket_accept` 启动 socket 绑定的 callback accept loop。
- 移除旧 `src/c/galay-{c,utils,http,ws,http2,redis,rpc,mysql,mongo,etcd,mcp,ssl,tracing}` 包装层源码，当前 C API 构建入口仅保留 `galay-kernel-c`。

### Fixed

- 修复 C coroutine C ABI 源码边界遗漏异常控制流门禁的问题：`coro_task_c` 和 `coro_wait_c` 不再使用 `try/catch` 兜底，wait request/timer 分配改为显式失败返回，并让 `t22_coro_source_boundaries` 覆盖 `coro-c` 异常 token 与 `std::make_shared`。
- 修复 C coroutine `AsyncWaiter` bridge 在 spawn/notify/destroy 竞态下的偶发 SIGSEGV：`await_suspend(false)` 路径补齐 `await_resume()` 清理语义，并避免 completion 恢复协程后继续访问已销毁的栈上 operation 回调。
- 修复 Linux epoll/io_uring 全量 examples/benchmarks 验证中的误报和真实失败：HTTP proxy/manual HTTP2 示例支持 build-root 与 Tencent `source` 布局下的静态文件/证书解析，SSL echo 与 TCP/SSL throughput 按 C/S 配对执行，etcd/MySQL/Redis/Mongo/RPC service-discovery 外部依赖被归类为 `EXTERNAL_DEP` 而不是未知失败。
- 修复 C kernel `coro_tcp` 在并发 CTest 下 close-while-waiting 子场景可能在 server 尚未进入可关闭阶段时启动 closer 的竞态，并为失败路径输出内部诊断码，避免远端日志只显示空输出。
- 修复 `benchmark_c_kernel_async_waiter_signal` 和 `benchmark_c_kernel_coro_tcp_iov_sendfile` 在 Linux smoke sweep 中 workload 过重导致崩溃/超时的问题，前者新增可校验的正整数迭代参数，矩阵脚本对二者传入短 workload。
- 修复 C async API reviewer 发现的 MySQL/SSL parity 缺口：MySQL C auth loopback 覆盖并实现 `caching_sha2_password` fast auth 与 RSA full auth exchange；SSL C API 补齐 ALPN offer/select、协商结果读取和 session cache/ticket/timeout context controls，并新增 loopback 覆盖。
- 修复 C Kernel AsyncMutex direct coroutine bridge 的 wake state 生命周期问题：`ResumeToken` 不再引用栈上 operation，改为引用计数堆状态，避免 waiter/waker 延迟释放时触发偶发 Bus error，并新增 512 轮 C handoff 压力回归覆盖。
- 修复 RPC managed client 清理路径静默丢弃返回值的问题：`release()` 与 `client.close()` 失败现在会通过 `RpcError` 显式传播，并新增源码边界测试防止回退到 `(void)` 忽略返回值。
- 修复 kernel timeout/C coroutine 边界：`WithTimeout` 处理 timer 注册失败返回值并立即传播错误，C TCP bridge 在 timeout 服务不可用时清理 awaitable/user_data 后返回错误，`AsyncWaiter`/`AsyncMutex` await_suspend 路径满足最终挂起状态发布约束。
- 修复 etcd t13/t14 cluster integration CTest 注册遗漏，未启用 `GALAY_IT_ENABLE` 时按 `SKIP_RETURN_CODE` 统计为 skipped 而不是失败。
- 修复 direct C coroutine TCP bridge 在 timeout timer 注册失败路径中未撤销 reactor registration 的生命周期问题，避免返回错误后后端仍持有栈上 awaitable 或悬空 controller；新增 C++ 回归测试覆盖 kqueue/epoll 清理与 socket 复用。
- 修复 HTTP writer timeout 错误码映射为 `kSendTimeOut`，并加固 HTTP 静态文件与代理路径的返回值处理，避免 close/文件系统错误被静默丢弃。
- 修复 C coroutine bridge 清理路径未完整传播返回值的问题，并加固 UDP bridge `user_data` 完成读取竞态，确保清理失败可合并为可观测错误而不是被静默丢弃。
- 加固 C/C++ no-exception 与函数返回值必须处理规则：移除 C async 测试、示例、benchmark 和 bridge 中的裸调用/void-cast 忽略返回值路径，保持错误通过返回结构或错误码传播。
- 修复 C Kernel timeout 示例合并冲突后的 cleanup 返回值处理，避免 direct coroutine 示例重复销毁任务句柄并保留清理失败错误码。
- 修复 epoll reactor 在 one-shot connect 完成后可能保留未注册删除 pending 的问题，避免 socket 析构后残留悬空 `IOController*` 导致后续 HTTPS/WSS closed-port connect 无法注册并卡住 `http.error_propagation`。
- 修复 kernel coroutine/resource 错误边界：`TaskAwaiter` 先绑定 continuation 再调度子任务，timeout 与 IO 完成做仲裁，`spawnBlocking()` 捕获 callable 异常并映射到 task error，非法 borrowed `readv/writev` count 返回 `IOError(kParamInvalid)` 而不是 abort。
- 修复 socket/file RAII、ObjectPool late lease、Base64 malformed input、`Bytes::c_str()` NUL 结尾等资源生命周期和可恢复错误问题。
- 修复 HTTP/WS/HTTP2/RPC/Redis/MySQL/Mongo/etcd/MCP/SSL/tracing 审计中发现的一批协议正确性、安全边界、错误传播和生命周期问题，并补齐对应 CTest 覆盖。
- 修复 sequence overflow abort、HTTP session oversized response、HTTP/2 send-window 下溢、MySQL 超大 packet 部分发送，以及 tracing batch processor shutdown 超时后的继续 drain/析构终止问题。
- 修复 C API MySQL/Mongo/tracing/HTTP2/Redis/etcd 包装层中残留的异常边界路径，统一改为显式返回码、空指针检查和 no-fail 分配检查。
- 修复 Redis/Rediss 连接池等待者 release、timeout、shutdown 之间的竞态，等待者完成状态只允许一次性转移，并修正 active/waiting 统计泄漏。
- 修复 C ABI TCP accept 在 pending 状态下无法可靠取消的问题，通过共享监听状态和本地 cancel 连接唤醒 accept，避免 destroy/join 卡住。
- 修复 C Kernel TCP socket C ABI 错误语义：`galay_kernel_tcp_socket_destroy(NULL)` 返回 `C_TcpSocketParameterInvalid`，async submit API 在 runtime 停止时统一返回 `C_TcpSocketRuntimeNotRunning` 而不是继续提交任务。
- 加固 C ABI kernel 生命周期边界：`test/c/kernel/t5_socket_lifetime_boundary.c` 覆盖 runtime、tcp/udp socket 与 accept loop 的空指针、重复销毁和停止请求边界。
- 加固 C++ `Host` 字符串构造与 TCP/UDP bind 边界：非法 IP 或协议类型会被标记为 invalid，bind 在系统调用前返回 `IOError(kParamInvalid)`，C ABI bind 将其映射为 `ParameterInvalid`。

### Docs
- 补齐 C public ABI 头文件 Doxygen 注释，覆盖 common/utils/kernel/bridge、HTTP/WS/HTTP2、Redis/MySQL/Mongo、Etcd/MCP/RPC、SSL/tracing 等模块的 ownership/lifetime、buffer 借用、错误码、coroutine 挂起、timeout/cancel/close 与线程/协程安全契约；本次仅更新头文件注释，不删除 public C ABI。
- 补齐 C async API 模块 README 与 public header 对齐说明，覆盖 HTTP request/response parser/builder、header helper、route/session ownership、WS/HTTP2/Redis/Mongo/MCP/RPC/kernel/tracing helper family，以及 MySQL/SSL 新增认证与 ALPN/session 语义。
- 新增 `docs/文档审查报告.md`，记录 docs 目录 354 个 Markdown 文件的结构、链接、命名和完整性审查结果及修复优先级。
- 更新 C Kernel 性能文档，按 C++ 性能文档结构记录 2026-06-25 Release fresh TCP/UDP 双进程 C/S 压测数据、timeout API pressure/smoke 输出、复现命令、target 清单和网络吞吐指标解释。
- 新增 `docs/c/modules/kernel` 文档导航与性能页，记录 C `TcpSocket` Release 构建、回归命令、同参数 C/C++ loopback benchmark 数据和当前复现口径。
- 按文档审查报告整改 docs 体系：统一全部模块 README 的 H1 标题（由「文档导航/索引/总览/首页」等多种写法改为「<模块> 文档」）；修复 http2/ws README 指向不存在文件的死链，仅保留实际可用文档入口。
- 修正 mongo 模块交叉引用编号错乱（`06-示例代码` → `04`、`07-高级主题` → `06`）；移除 redis/ssl/http 快速开始中硬编码的开发者私有路径，统一用 `<galay-install-prefix>` 占位。
- 校正 redis/http 快速开始中的示例源码路径与 target 名称，使其与当前 monorepo `examples/cpp/...` 目录结构和 target 命名一致。
- 新增 mongo 模块 README 与 `docs/README.md` 顶层文档索引入口；更新 `docs/文档审查报告.md`（2026-06-29），跟踪前序问题修复状态并补充硬编码路径、示例不可运行等新发现。
- 同步 `docs/release_note.md` 中的模块文档路径（`docs/modules/` → `docs/cpp/modules/` 与 `docs/c/modules/`）。

### Chore

- `.gitignore` 新增 `.workbuddy/` 工作区记忆目录忽略规则，避免本地工作日志进入提交范围。
- 恢复 `src/c/CMakeLists.txt` 最小入口，`GALAY_BUILD_C_API=ON` 时只注册当前保留的 `galay-kernel-c` target。
- 隐藏 C Kernel TCP socket C wrapper 内部 helper 与 coroutine task helper 的外部链接符号，减少 `libgalay-c-kernel` 符号污染。

## [v3.0.0] - 2026-06-22

### Added

- **新增 kernel TCP accept C ABI 异步接口**：`galay_kernel_tcp_accept_{start,wait,join,destroy}` 通过 runtime 调度的 `JoinHandle<AcceptResult>` 暴露异步 accept，`galay_kernel_tcp_socket_{bind,listen,local_endpoint}` 补齐服务端建链步骤，并经 peer/local host config 返回 IPv4/IPv6 地址与端口。
- **新增 C ABI TCP accept 用例**：`test/c/kernel/t4_tcp_accept_api.c`（参数校验 + 完整建链到 accept 流程）、`examples/c/kernel/e2_tcp_accept.c`（含 POSIX 阻塞客户端的端到端示例）、`benchmark/c/kernel/b2_tcp_accept_smoke.c`（accept smoke 基准），并在对应 `CMakeLists.txt` 注册 CTest 入口。
- **新增 IPv6 dual-stack socket 选项**：`HandleOption::handleIPv6Only(bool)` 显式设置 `IPV6_V6ONLY`；`TcpSocket::openHandle` / `UdpSocket::openHandle` 在 IPv6 场景默认调用 `handleIPv6Only(false)` 启用 dual-stack，并新增 `test/cpp/kernel/t127_ipv6only.cc` 锁定源码与运行时行为。
- **补齐 galay-rpc 生产级能力**：新增调用级 metadata/options、错误码扩展、连接级 `RpcChannel`、并发 unary、deadline/cancel、heartbeat、reconnect、连接池、托管客户端、重试/治理/背压、配置/endpoint cache、etcd registry contract、stream 契约加固、server interceptor/TLS hook、metrics/tracing helper 等公共能力，并同步导出到 module facade。
- **新增 RPC 边界测试与压测矩阵**：补齐 malformed/truncated/oversized 协议帧、borrowed payload、metadata wire round-trip、并发 unary、deadline/cancel、heartbeat/reconnect、连接池、托管客户端、治理/背压、stream、auth、TLS、metrics/tracing、综合边界矩阵等 CTest 覆盖；新增 unary latency、stream pressure、concurrent unary、pool pressure、managed client、payload scaling 等 benchmark。
- **新增 RPC release benchmark 与开源对比脚本**：新增 `scripts/rpc/301_rpc_release_benchmark.sh`、`scripts/rpc/302_rpc_compare_open_source.sh`、`benchmark/cpp/rpc/README.md` 与 `docs/modules/rpc/performance-comparison.md`，记录 release 模式 QPS/latency、错误数和本地缺少开源 C++ RPC 基线工具链时的明确阻塞信息。
- **新增 C ABI 包装层 `src/c/`**：覆盖 utils/kernel/ssl/http/ws/http2/redis/rpc/mysql/mongo/etcd/mcp/tracing 共 13 个模块，以及通用 `galay-c` 包（含错误码与 ABI 宏），共 44 个文件；通过新增的 `GALAY_BUILD_C_API=ON` 构建选项启用，与既有 C++ 构建互不干扰。
- **新增 C ABI 用例目录**：`benchmark/c/`、`examples/c/`、`test/c/`（共 99 个文件）提供各模块 C ABI 的 codec/builder/lifecycle smoke 基准、示例与回归测试入口。
- **测试集成配置头**：新增 `test/cpp/{etcd,redis}/integration_config.h`，作为对应模块集成测试的统一配置入口。
- **tracing HTTP header / OTLP galay http transport 集成测试与基准**：新增 `test/cpp/tracing/t14_http_header_integration.cc`（验证 tracing adapter 与 galay-http header 互转，需 `galay::http`）、`t15_otlp_galay_http_transport_integration.cc`（验证基于 galay http 的 OTLP transport 行为，需 `GALAY_TRACING_ENABLE_GALAY_HTTP_OTLP_TRANSPORT`）以及对应吞吐基准 `benchmark/cpp/tracing/b9_http_header_integration.cc`；同步把原 `t9_otlp_http_exporter.cc` 中与 galay-http-transport 强绑定的用例迁入 t15，让 t9 回归纯 OTLP HTTP exporter 单元测试。
- **真实 etcd 服务发现集成**：`EtcdServiceRegistry` 在构建启用 `galay-etcd` 时走真实 etcd v3 KV 作为注册中心（`GALAY_RPC_HAS_ETCD`），覆盖 register/deregister/discover/heartbeat/integrationAvailable，支持 `{prefix}/{service}/{instance}` key 模板与脏值跳过；新增真实链路集成测试 `test/cpp/rpc/t64_etcd_real_chain.cc` 与压测基准 `benchmark/cpp/rpc/b12_etcd_managed_client_pressure.cc`，并按 `GALAY_IT_ENABLE` + `GALAY_ETCD_ENDPOINT` 门控。
- **RPC C ABI 错误码扩展**：`galay_rpc_error_code_t` 新增 CANCELLED / DEADLINE_EXCEEDED / RESOURCE_EXHAUSTED / RATE_LIMITED / CIRCUIT_OPEN / UNAUTHENTICATED / PERMISSION_DENIED / UNAVAILABLE 八个错误码，并对齐 `galay_rpc_error_to_status` 映射。
- **新增 await_suspend 竞态 / discovery selector / stream control body 回归测试**：`test/cpp/kernel/t126_await_suspend_race_source.cc`（源码边界，锁定 AsyncWaiter/Mutex/MpscChannel 的 await_suspend 只消费提前唤醒、不触碰 awaiter frame）、`test/cpp/rpc/t63_discovery_selector.cc`（round-robin 位置跨调用保持与 weighted selector 构造）、以及 `t70_stream_contract.cc` / `t1_protocol.cc` / `t1_envelope_codec.c` / `t100_boundary_matrix.cc` / `t41_managed_client.cc` 的相应扩充。
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
- 新增 HTTP/2 HEADERS-only 静态空响应 fast path，GET/HEAD exact path 命中时可绕过 active handler 和完整 stream 生命周期，并复用预编码响应头。
- 新增 HTTP/2 小 body 静态响应 bytes fast path，GET 命中时批量发送预编码 HEADERS 与 DATA bytes，HEAD 只返回响应头。
- 新增 HTTP/2 静态文件 metadata/cache 组件，支持 path 规范化防逃逸、404、小文件 body 缓存、MIME、ETag 与 If-None-Match 304。
- 新增 HTTP/2 静态空响应 h2load benchmark server 与 `scripts/http2/300_http2_h2load_compare.sh --galay-static-empty` 模式，记录 req/s、p95、p99、CPU、RSS 与失败率。
- 新增 `scripts/http2/300_http2_h2load_compare.sh --galay-static-small`，记录 1KB 静态响应 fast path 的 h2load 指标。
- 新增 HTTP/2 `H2StaticResponse`/`H2StaticRoute` 静态响应配置类型，以及 h2c/h2 server builder 的 `staticResponse()` 配置入口。
- 新增 `scripts/http2/300_http2_h2load_compare.sh`，记录 galay h2c POST echo 与 `nghttpd --echo-upload` 的同参数外部 h2load 对比基线。
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

- **TcpSocket / UdpSocket 新增 `create(IPType)` 静态工厂**：返回 `std::expected<..., IOError>` 显式报告错误；原构造函数降级为兼容重载，失败时句柄保持 invalid；私有助手 `create` 更名为 `openHandle`，并在 IPv6 场景默认调用 `handleIPv6Only(false)` 启用 dual-stack。
- **Scheduler / Runtime 启动边界改为 `std::expected`**：`Scheduler::start()` 与 `ComputeScheduler::start()` 返回 `std::expected<void, IOError>`；`Runtime::start` / `ensureStarted` / `acquireDefaultScheduler` 返回 `std::expected<..., RuntimeError>`，新增 `RuntimeErrorCode::kSchedulerStartFailed`，启动失败自动 `stop()` 已启动的 scheduler。
- **Reactor 抽象改为 `ReactorType` concept**：移除虚基类 `BackendReactor`，改为基于 `notify()` + `getHandle()` 的编译期约束，epoll/kqueue/io_uring 三后端通过 `static_assert` 锁定；`wakeReadFdForTest()` 更名为 `getHandle()`，新增 `std::expected<void, IOError> start()` 显式初始化。
- **RPC 请求协议支持可选 metadata 扩展**：在请求体前增加向后兼容的 metadata marker 编码，旧格式请求仍按原 service/method/payload 解码；客户端真实 writev 发送路径和 direct serialization 保持一致，server interceptor 可读取真实跨网络 metadata。
- **RPC 通道生命周期加固**：reader/writer/cancel watcher 统一纳入后台任务计数，`close()` 等待所有后台任务退出后返回；pending 计数改为原子快照，避免诊断读取与分发表更新竞争。
- **RPC C++23 module file set 显式门禁**：`rpc.t92.module.smoke` 仅在 `galay-rpc` / `galay::rpc` 目标真实注册 C++ module file set 时生成，当前 AppleClang 路径明确跳过。
- **优化 RPC 热路径**：为 pending response/heartbeat dispatch 表按 `max_in_flight` 预留容量，并减少连接池重复 endpoint lookup，降低高并发 unary 和 pool pressure 场景中的分配与 hash 成本。
- **源码目录归入 `src/cpp/`**：将 `src/` 下各模块（`galay-utils`/`kernel`/`ssl`/`http`/`ws`/`http2`/`redis`/`rpc`/`mysql`/`mongo`/`etcd`/`mcp`/`tracing`）统一迁入 `src/cpp/` 子目录，共 421 个文件纯移动，为后续多语言绑定预留 `galay/cpp/` 命名空间。
- **头文件 include 路径统一**：所有 benchmark 与 test 源文件的 `#include "galay-xxx/..."` 改为 `#include <galay/cpp/galay-xxx/...>`，顶层 `CMakeLists.txt` 的 `add_subdirectory` 同步指向 `src/cpp/galay-*`，并在构建目录通过符号链接 `${CMAKE_BINARY_DIR}/include/galay/cpp -> src/cpp` 提供统一 include 根；头文件安装目录改为 `${CMAKE_INSTALL_INCLUDEDIR}/galay/cpp`。
- **测试/基准 include 根调整**：各 benchmark/test 的 CMakeLists 将私有 include 目录由 `${PROJECT_SOURCE_DIR}/src` 改为 `${CMAKE_BINARY_DIR}/include`，对齐新的符号链接布局。
- **benchmark/examples/test 按语言分层**：三者的各模块子目录整体迁入 `cpp/` 子目录（如 `benchmark/kernel/` → `benchmark/cpp/kernel/`，examples/test 同），共 637 个文件纯移动；对应 `CMakeLists.txt` 由 `add_subdirectory(<module>)` 改为 `add_subdirectory(cpp/<module>)`，并在 `GALAY_BUILD_C_API=ON` 时额外 `add_subdirectory(c)`。
- **顶层 `CMakeLists.txt` 启用 C 语言**：项目 `LANGUAGES` 改为 `C CXX`；`GALAY_BUILD_C_API=ON` 时新建 `${CMAKE_BINARY_DIR}/include/galay/c -> src/c` 符号链接、`add_subdirectory(src/c)`、安装 C 头文件到 `${CMAKE_INSTALL_INCLUDEDIR}/galay/c`，并生成 13 个 `galay-c-*` CMake config-package。
- **新增 `GALAY_BUILD_C_API` 选项**：`cmake/option.cmake` 增加 ABI 构建开关（默认 OFF），不影响现有 C++ 默认构建行为。
- **测试用例统一重编号**：etcd/http/http2/kernel/mcp/mysql/redis/rpc/ssl/ws 共 10 个测试目录的 `t{n}_*.cc` 改为从 `t1` 起连续编号；同步更新 etcd/kernel/mcp/mysql/redis/ssl 各自 `CMakeLists.txt` 中硬编码的集成测试/TLS 测试/场景名清单；修复 kernel 目录 `t116_sqestatesrc.cc` 与 `t116_connfan.cc` 的序号冲突（冲突项起整体后移一位至 t125）。
- **测试 CMake 改用 GLOB 自动发现**：mcp/mongo/tracing/utils 四个目录不再逐个 `add_executable`/`add_test`，统一改为 `file(GLOB ... CONFIGURE_DEPENDS)` + 循环派发；保留各模块原有的集成/单元分类、链接库差异、tracing 的 `T1-package_surface` 目标命名与 `t6_spdlog_sink` 条件编译等特例。
- **具体 IOScheduler 关闭 sibling work-stealing**：epoll/kqueue reactor 的事件注册与删除必须保持 owner 线程亲和，被窃取的 IO 协程会在错误线程触碰 reactor；`EpollScheduler` / `KqueueScheduler` 构造时显式 `m_worker.setStealingEnabled(false)`，`t99_iosteal.cc` 由"验证会偷"改写为"验证不偷"，`t104_iouoffsrc.cc` 同步锁定 kqueue/epoll 源码边界。
- **ServiceDiscoveryClient selector 状态跨调用保持**：selector 改为 `std::optional<Selector>` 并缓存对应 endpoint 快照，仅当 endpoint 列表真正变化时才重建；同时通过 `if constexpr` 探测 selector 是否接受 `(endpoints, weights)` 构造，兼容加权选择器。
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

- 移除旧的单体目录 `galay/`（含 `algorithm`、`common`、`kernel`、`utils` 及其全部子目录）。
- 移除旧的 `test/`、`benchmark/`、`doc/` 目录与旧的 `README.md`、`README_CN.md`、`LICENSE`、`GalayConfig.cmake.in`。
- 移除旧访问器类。
- 移除 http2 相关旧函数。
- 移除 `galay-http/server/file_descriptor.h`，HTTP 静态文件发送路径改为直接使用 kernel 层 `FileDescriptor`。

### Fixed

- **C ABI tcp/udp socket create 移除 try/catch**：`galay_kernel_tcp_socket_create` / `galay_kernel_udp_socket_create` 不再依赖异常映射，改为直接调用新 `create()` 工厂并按返回的 `IOError` 映射错误码，`galay_kernel_runtime_{create,start,stop,destroy}` 同步去掉 try/catch 并消费 `Runtime::start` 的 expected 返回。
- 修复 RPC metadata 只存在于 `RpcCallOptions` 容器、未随请求跨网络传输的问题；同时限制 metadata value 最大长度，避免 wire 编码中的 `uint16_t` 长度字段静默截断。
- 修复 RPC 请求 parser 对未知 reserved bit 缺少明确拒绝的问题；metadata 扩展位现在作为已知 reserved bit 处理，其它未知扩展位返回 `INVALID_REQUEST`。
- 修复 RPC etcd discovery 示例把 `RpcError::message()` 误当字段访问导致全量构建失败的问题。
- 修复 etcd benchmark CMake glob 把 `bench_support.cc` 注册成独立可执行目标导致缺少 `main` 链接失败的问题。
- 修复 kernel IO scheduler 的 work-stealing ring 槽位复用竞态，避免跨线程注入压力下 ready task 被覆盖丢失，并同步修正 kqueue/epoll/io_uring 注入失败返回；同时修正 TCP benchmark source-case 测试的 `benchmark/cpp/kernel` 路径。
- 修复 `AsyncWaiter` / `AsyncMutex` / `MpscChannel` 在 await_suspend 与 notify/wakeUp 之间的双恢复与丢失唤醒竞态：`AsyncWaiter` 改为 `kEmpty/kWaiting/kReady` 状态机 + 独立 `m_notified` 标志，`AsyncMutex` 发布 waiter 后只使用栈上本地副本并经 `wakeNextWaiterIfUnlocked` 转交锁，`MpscChannel` 注册 waiter 后只消费提前唤醒、不再同步收数据并写 awaiter frame。
- 修复 `WaitRegistration` 在唤醒先于 waiter 注册到达时丢失唤醒的问题：新增 `m_pending_wake` 标志与 `consumePendingWake` / `clearPendingWake` 接口，`arm()` 未拿到 waiter 时记一次 pending，`publishWaiter` / `tryRecv` / `tryRecvBatch` 在对应路径消费或清理。
- 修复 `RpcClient::call` / `RpcManagedClient::call` 在挂起协程中持有栈上 borrowed payload 的 use-after-free：payload 与 service/method 统一拷贝进 owned 存储 (`copyPayload` / `callWithModeOwned` / `callOwned`) 再进入重试与调度路径。
- 修复 RPC stream 控制帧 (`STREAM_END` / `STREAM_CANCEL`) 携带 body 时未被拒绝、并污染后续帧解析的问题：新增 `DiscardInvalidControlBody` 状态，body 不完整时缓存已消费 header、就绪后整体丢弃并返回 `INVALID_REQUEST`。
- 修复 `RpcHeader::deserialize` 未校验协议版本，以及 C ABI `galay_rpc_decode_request` 未跳过 metadata 扩展、未拒绝未知 reserved bit 的问题。
- 修复 `coroutine.wait` 协程状态问题。
- 修复 mpsc 队列 batch 操作无法唤醒的 bug。
- 修复 `AsyncResult` 框架中的关键 bug。
- 修复 SSL 上下文管理与错误处理，每个 SSL 实例支持独立 ssl_ctx。
- 修复 io_uring 宏与 linux aio 事件的编译报错。
- 修复头文件依赖告警。
- 修复协程状态竞态问题。
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

- kernel 模块常见问题文档中的测试日志头路径更新为 `test/cpp/common/stdout_log.h`，与新分层目录对齐。
- 新增项目 `README.md`，介绍 galay 特性、13 个 `galay-*` 模块、环境要求、CMake 快速开始与目录结构。
- 补充 HTTP/2 静态文件 Release 对比校正文档，明确非 Release 构建不能与 Homebrew `nghttpd` 发布版作公平性能结论，并记录 0B/1KB 静态文件同参数 h2load 对照。
- 新增 HTTP/2 性能测试文档，记录 kernel 压测环境、复现命令、QPS/MiB/s 指标、真实瓶颈和后续优化方向。
- 新增 HTTP/2 dispatcher/scheduler 生产级优化计划并按任务勾选执行进度。
- 新增 MySQL 认证插件真实服务端验证说明，记录本机测试用户创建、集成测试运行和清理流程。
- 新增 `CLAUDE.md` 与 `AGENTS.md`，定义 LLM 代理在本仓库内的行为准则（编码前先思考、简洁优先、外科手术式修改、目标驱动执行），降低常见编码错误。

### Chore

- **拆分 benchmark/examples 与 test**：将原本散落在 `test/cpp/` 下的基准与示例源码迁出到 `benchmark/cpp/*/b*.cc`（etcd/redis/ssl）与 `examples/cpp/*/include/manual/*.cc`（http/http2/redis/rpc/ws），让回归测试目录只保留真正的 `t{n}_*.cc` 用例。
- 新增 `scripts/mysql/202_mysql_auth_matrix_setup.sh`，按模块目录管理 MySQL auth 矩阵测试用户准备脚本。
- 扩充 `.gitignore`，新增 `.claude/`、`.codex/` 条目，避免代理本地配置目录进入版本控制。
- 扩充 `.gitignore`，新增 `docs/modules/*/plans`，避免按模块拆分的本地规划文档进入版本控制。
- 移除 examples/tests/benchmarks/scripts style 审计中的 `stale-include-root` 阻断规则，保留其他结构与命名检查。
