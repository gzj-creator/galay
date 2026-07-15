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
