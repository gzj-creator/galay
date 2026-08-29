# Changelog

本项目所有显著变更均会记录在本文件中。

## 如何维护

- **版本规则**：稳定版本遵循 `major.minor.patch`，预发布版本在其后追加 `-alpha`、`-beta` 或 `-rc.N` 后缀。大改动（架构/目录重组/核心接口变更）升 `major`；新增功能升 `minor`；修复 bug、文档、配置、chore 等小修补升 `patch`。
- **更新时机**：每次提交前都必须更新本文件。未发版的变更写入 `## [Unreleased]` 节；发版时把 `Unreleased` 收束为新的版本节，并在最上方补一个空的 `## [Unreleased]`。
- **标题格式**：`## [vX.Y.Z] - YYYY-MM-DD`；预发布可使用 `## [vX.Y.Z-beta] - YYYY-MM-DD`。
- **内容粒度**：按 `Added` / `Changed` / `Removed` / `Fixed` / `Docs` / `Chore` 等小节归纳，只记录最重要的变更，不逐行抄写 diff。

## [Unreleased]

## [v5.0.1] - 2026-08-30

### Fixed

- **收紧模块 prelude 的 intrinsic 头文件边界**：`intrin.h` 仅在 MSVC
  ABI 下预包含，`emmintrin.h` 仅在 x86 架构下预包含，避免 Clang/GCC
  非目标平台的转发头或 TU-local 定义进入 C++23 模块导出范围。
- **补充模块 prelude 回归测试**：新增生成器与已生成 prelude 的平台守卫检查，
  防止后续生成结果重新引入无条件 intrinsic 头文件。

### Changed

- **同步三套构建版本元数据**：CMake `project()`、Bazel `module()` 与 mcpp
  `[package]` 版本统一更新至 `5.0.1`，与本次小版本 tag 对齐。

## [v5.0.0] - 2026-08-29

### Changed

- **统一 Redis 异步客户端入口**：将普通 Redis 与 Rediss/TLS 客户端合并到
  `async/client.h` / `async/client.cc`，保留原有 C++ API，并通过
  `GALAY_SSL_FEATURE_ENABLED` 控制 TLS 实现编译。
- **调整 Redis feature 构建边界**：`redis` 负责全部普通 Redis 源码，`redis + ssl`
  自动同时构建普通 Redis 和 Rediss；移除独立的 `redis-tls` feature/target，Redis-only
  构建不再引入 OpenSSL。
- **同步三套构建版本元数据**：CMake `project()`、Bazel `module()` 与 mcpp
  `[package]` 版本统一更新至 `5.0.0`，与本次主版本 tag 对齐。

### Fixed

- **修正 mcpp Linux 链接方式**：将 Galay mcpp 目标改为静态归档并移除
  `mcpp/libaio.map` 版本脚本，避免最终消费者无法解析 libaio 默认 `@@LIBAIO_*`
  符号。

### Removed

- **移除拆分的 Redis 客户端文件**：删除 `async/redis_client.h`、
  `async/redis_client.cc` 和 `async/client_tls.cc`，统一使用 `async/client.h` 与
  `async/client.cc`。

## [v4.10.0] - 2026-08-29

### Added

- **内置 GCC 16 兼容的 simdjson**：纳入 simdjson v4.6.9 单头文件与静态实现，
  通过 `simdjson::simdjson` 项目自有目标构建，并以 `galay::simdjson` 导出，
  消除 galay-mcp/galay-etcd 对宿主 simdjson 版本的依赖。
- **模块 prelude 生成器**：新增 `scripts/gen_module_prelude.py`，从模块接口的
  include 依赖自动生成或校验全局模块片段，减少模块接口与 prelude 漂移。

### Changed

- **统一 C++23 模块接口边界**：各模块接口使用显式 `export extern "C++"`，并
  将标准库、系统头、第三方头和跨模块头在全局片段中展开；安装时同步提供全部
  `.cppm` 与 `module_prelude.hpp`。
- **收敛第三方安装布局**：simdjson 与 concurrentqueue 均安装到
  `galay/thirdparty`，模块和普通头文件消费路径保持一致。

### Fixed

- **修复 GCC 16 模块 CMI 序列化**：将 simdjson formatter 使用的匿名命名空间
  helper 调整为 `simdjson::internal` 下的 inline linkage，解决 `galay.mcp`
  模块无法写出 CMI 的问题；解析、格式化算法和公开 API 保持不变。
- **修复模块导出与普通库 ABI 不一致**：将跨模块公共常量改为 `inline constexpr`，
  补齐 HTTP/2 客户端 awaitable 的自包含声明，并修正 tracing 基础 logger 命名
  冲突。

## [v4.9.5] - 2026-08-29

### Added

- **内置 concurrentqueue 依赖**：将 moodycamel concurrentqueue 头文件、许可证与说明纳入源码包，并通过统一的 `galay/thirdparty/concurrentqueue` 安装布局提供给 CMake 与 Bazel 消费者。

### Changed

- **统一第三方头文件引用**：Galay C++ 源码、模块 prelude、tracing benchmark 与安装布局测试统一使用仓库内置的 `galay/thirdparty/concurrentqueue` 路径，构建不再依赖主机上的 concurrentqueue 包。
- **同步构建版本元数据**：CMake `project()` 与 Bazel `module()` 版本均更新至 `4.9.5`。

### Fixed

- **修复 GCC 模块预包含边界**：模块 prelude 预包含 `mm_malloc.h`，避免 x86 intrinsic 辅助定义落入导出模块作用域导致编译问题。

## [v4.9.4] - 2026-08-28

### Added

 - **新增结构化并行 DAG**：提供 `ParallelGraph` / `parallel(...)`，支持依赖边、拓扑环检测、同步 work item、失败排空与全部节点进入 terminal 后再恢复父协程；新增 `t181_parallel_dag` 边界测试和 `b35_parallel_work_item` 协程开销对照基准。
 - **补充并行停机竞态回归覆盖**：新增 `t182_parallel_shutdown_races` 与 `b36_parallel_scheduler_shutdown_admission`，验证停机并发提交不丢已接纳工作，以及父任务恢复失败能够通过完成态观察。

### Changed

 - **统一并行调度器命名与归属**：`ComputeScheduler` 迁移至 `galay-kernel/parallel/parallel_scheduler.{h,cc}` 并重命名为 `ParallelScheduler`，Runtime、C/C++ 配置字段、Builder、亲和性字段、模块入口、示例、测试、benchmark 和文档同步改为 `parallel` 命名；旧头文件、旧类型和旧配置字段不再保留。
 - **补充非协程计算工作队列**：`ParallelScheduler` 新增 `ParallelWorkItem` 入队路径，普通同步 `noexcept` work 不再为每个节点创建 `Task<void>` coroutine frame。
 - **收敛 C++23 模块构建与安装分流**：默认关闭原生模块扫描并开启 `.cppm` 接口安装；仅 kernel、postgres、rpc 使用原生模块文件集，其余模块固定安装接口；RPC 在不安装接口时通过独立 OBJECT 目标保持模块依赖可见，并补齐 etcd 模块宏应用。
 - **同步发布版本元数据并统一 C++23 模块开关**：CMake 与 Bazel 版本更新至 `4.9.4`，移除重复的 `ENABLE_CPP23_MODULES` 别名及相关旧文档引用，统一使用 `GALAY_ENABLE_CPP23_MODULES`。

### Fixed

 - **修正 TaskPromise 协程分配失败回调**：统一 `TaskPromise<T>` 与 `TaskPromise<void>` 的标准回调名称和静态接口，失败时返回对应的无效 `Task`，并补充公开任务 API 的编译期契约检查。
 - **优化 C++ 协程帧生命周期与分配**：为 `TaskPromise<T>` / `TaskPromise<void>` 补齐普通、sized、aligned 和 sized+aligned 分配释放入口，引入按 128/256/512/1024/2048 字节分桶的线程局部有界 recycler；未提交 frame、跨线程释放、超大帧和超对齐请求均走明确的生命周期或全局 fallback 路径，并新增边界测试与压力 benchmark。
 - **继续收敛 C++ 协程热路径**：完成态 TaskState 使用 teardown fast path，普通对齐 frame 使用 provenance header 回收，promise 内部改用 raw TaskState view，TaskState/frame TLS cache 上限降为每桶 256；保留仅用于边界验证的 detail 测试 hook，并新增可参数化的 `b35` 百万/千万级压力 benchmark。
 - **修复并行调度停机与图错误传播**：普通任务和 `ParallelWorkItem` 使用提交接纳协议排空停机竞态；owner scheduler 无法恢复 parent 时将 `kResumeFailed` 写入任务完成态并映射到 `RuntimeError`；图节点和依赖存储改为无抛增长并通过 `kAllocationFailed` 显式返回。
 - **收敛并行恢复失败错误传播**：移除不可由正常协程生命周期到达的 `ParallelErrorCode::kResumeFailed` awaiter 防御分支，保留任务完成态与 `RuntimeError` 中真正可观察的恢复失败错误。
 - **修复协程帧释放的尺寸来源问题**：释放时使用分配头记录的真实 bucket，避免错误 sized-delete 参数将 frame 放入过大的缓存桶；恢复 frame/TaskState 分配失败和缓存容量的边界观测覆盖。

### Docs

 - **统一并行调度相关注释语言**：将本次并行调度、协程帧分配、停机竞态及边界测试和 benchmark 中的自然语言注释统一为中文，保留 API、类型和必要技术术语。

## [v4.9.3] - 2026-08-26

### Removed

- **清理源码包中的非必要资产**：移除不应提交的本地 benchmark/test 汇总、未被仓内构建或测试引用的 HTTP/2 与 WebSocket 静态 fixture、副本测试文件，以及未被构建脚本引用的 `libwebsockets` 压缩源码包；保留 HTTP 路由和代理示例实际使用的 canonical static fixture。

### Chore

- **补充临时报告忽略规则**：将 `benchmark_run_results.txt` 与 `test_failure_summary.md` 加入根目录 `.gitignore`，避免本地验证输出再次进入提交。

## [v4.9.2] - 2026-08-26

### Fixed

- **收敛 HTTP 路由错误处理**：移除基于错误消息文本的断开连接猜测，改为仅依赖明确错误码；同时将响应发送失败和连接关闭失败提升为错误级别日志。CMake 与 Bazel 构建版本同步更新至 `4.9.2`。

## [v4.9.1] - 2026-08-26

### Added

- **统一 HTTP 静态文件异步读取器**：新增 `StaticFileReader` / `StaticFileSession`，统一 metadata、内存读取、分块读取和 range 读取接口；io_uring 使用 `AsyncFile`，其他后端将阻塞文件操作转移到 blocking executor。新增 `t90_static_file_reader` 覆盖 metadata、完整读取、范围读取、session 复用、sendfile descriptor 和错误边界。
- **HTTP 静态文件路径迁移覆盖**：静态文件的 MEMORY、CHUNK、单 range 与多 range 发送路径统一复用 reader，避免在 I/O scheduler 线程直接执行同步 open/read/lseek。
- **TimeoutTimer 调度线程本地对象池**：新增 `TimeoutTimerPool`，通过 `TimeoutTimer::create()` 从线程本地 free-list 获取或新建定时器，`resetForReuse()` 在复用前清零全部状态；跨线程最后释放直接析构，避免把对象发布到错误的池。新增 `t177_timeout_timer_pool` 验证池复用、跨线程析构与容量上限。
- **内核可调参数集中定义头**：新增 `kernel_config.h`，统一存放 `GALAY_KERNEL_TIMER_WHEEL_TICK_NS`、`GALAY_KERNEL_IO_POLL_IDLE_TIMEOUT_MS`、`GALAY_SCHEDULER_MAX_EVENTS` 等全部运行时宏，移除 epoll/kqueue/io_uring scheduler 中的重复 `#ifndef` 定义。
- **TCP 公平对标读路径接入 10ms 接收超时**：galay `b31` 三处 `readExact` 改用 `.timeout(10ms)` 并按超时计数重试；Boost.Asio TCP baseline 新增 `parallel_group`+`steady_timer` 竞速版 `readExactWithTimeout`，两侧策略严格对称。meta 输出新增 `recv_timeout_ms` 与 `recv_timeouts` 字段以暴露超时触发率。
- **Awaitable Timeout 压测后优化计划 Task D**：计划文档新增 D1（削减挂起路径 per-op 定时器注册成本，目标 @100ms 形态损耗 ≤8%）与 D2（定位饱和负载下 >10ms 事件分派停顿，目标触发率 ≤1%），并明确重试型超时压测结论在 D2 收敛前不得归档为正式证据。

- **惰性 TimeoutTimer 与 ready 路径零分配**：`WithTimeout` 改为在 `await_suspend` 中惰性创建 `TimeoutTimer`，`await_ready()` 为真的路径完全不分配定时器；ready 快路径吞吐约 24M ops/s，是 eager timer 创建路径的 4 倍。
- **显式 TimeoutPolicy 模板策略**：新增 `TimeoutSupport<Derived, TimeoutPolicy>` 与 `WithTimeout<Awaitable, TimeoutPolicy>` 双模板参数，新 awaitable 可通过 `Policy::inject()` 与 `ownsIoRegistration()` 自定义超时注入行为，编译期内联无虚函数开销。
- **SequenceAwaitableBase 完成去重与超时仲裁**：新增 `m_completed` 去重标志，`onCompleted()` 在发布唤醒前先 `cancelBoundTimeoutTimer()`，避免恢复排队延迟让已成功的 I/O 被滞后的 `TimeoutTimer` 误判为超时。新增 `t179_sequence_completion_once` 验证重复调用安全。
- **ForwardingAwaitable CRTP/owning 双形式**：统一 facade 的 `await_ready`/`await_suspend`/`await_resume`/`markTimeout` 转发，已收口 RPC、MySQL、PostgreSQL、Redis 全部协议 facade，消除复制粘贴样板。
- **自定义 awaitable 超时策略示例与测试**：新增 `e12_policy.cc` 示例、`t148_custom_awaitable.cc` 与 `t149_timeout_policy_surface.cc` 定向测试，验证显式策略编译期契约与 `ownsIoRegistration` 定制点。
- **TimeoutReadyPath 基准测试**：新增 `b33_timeout_ready_path.cc`，对比 ready 快路径与 eager timer 创建的固定开销。

### Changed

- **运行时阻塞任务收尾顺序**：`BlockingExecutor` 新增显式 `stop()`，Runtime 在停止 compute/IO scheduler 前先排空阻塞任务，确保异步 completion 的唤醒不会丢失；`AsyncFile::adopt()` 用于接管 blocking executor 打开的文件描述符。
- **poll 超时计算统一收归 IOScheduler**：epoll/kqueue/io_uring 三个后端的 poll 超时由 `schedulerPollTimeoutNanoseconds()`、`schedulerPollTimeoutMilliseconds()`、`schedulerPollTimeoutIoUringNanoseconds()` 统一计算，空轮使用 idle 上限，非空轮对齐下一个 tick 边界（`nsToNextTickBoundary()`），消除散落在各后端的 `halfTickPoll*` 辅助函数。新增 `t178_scheduler_poll_timeout` 验证空轮/非空轮/上限三路径。
- **TimeoutSupport 重命名为 TimeoutMethods**：所有继承 `TimeoutSupport` 的 awaitable 统一迁移至 `TimeoutMethods`，`SequenceAwaitableBase` 新增 `TimeoutTimerBinding` 支持超时绑定转发；HTTP2/SSL/WS facade 的 `await_suspend()` 补齐 `cancelBoundTimeoutTimer()` 与 `forwardBoundTimeoutTimer()` 调用。
- **io_uring reactor 完成路径统一超时仲裁**：所有 IO 类型的 `wakeUp()` 调用前统一通过 `completeAndWake()` 先 `cancelBoundTimeoutTimer()`，防止超时与正常完成的竞争；sequence 的 `onCompleted()` 在 `wakeUp()` 前调用。
- **epoll reactor 完成路径统一超时仲裁**：one-shot 与 FILEWATCH 事件在 `wakeUp()` 前先 `cancelBoundTimeoutTimer()`。
- **C io_scheduler ready queue 重写为无锁 MPSC**：生产者改为原子 CAS 堆叠的 LIFO 节点，消费者通过 `refill_pending()` 批量翻转为 FIFO 列表；新增 `reactor_inflight` 原子计数器，`unregister()` 等待当前 reactor 批次（含所有 slot 交换与任务唤醒）完成后才允许释放 controller。
- **C++ WS client socket 所有权提前移交**：`galay_ws_client_connect()` 在首次异步操作前就把 socket 移入 connection 对象，避免 stack socket 在 reactor 注册后被移动导致 epoll 指向已过期的 coroutine-stack controller。
- **UDP 公平对标收发超时统一为 10ms**：galay `b6` 客户端/服务端接收超时由 50ms/100ms 收敛为 10ms；Boost.Asio UDP baseline 服务端 `kServerReceiveTimeout` 与客户端硬编码超时同步收敛，消除双侧口径不一致。

- **TimeoutTimer 完成状态机收窄为唯一 Completion 原子操作**：`timeouted()` 改为直接读取 `m_completion == kTimeoutWon`，移除冗余 `m_flag | kTimeout` 写入；所有 `seq_cst` 内存序收窄为 `acq_rel`/`acquire`。
- **awaitableStillOwnsIORegistration 增加显式定制点**：优先检测 `ownsIoRegistration()` 方法，fallback 到 `m_controller` 指针比较，新 awaitable 可精确声明 IO 注册归属。
- **Channel/Sequence awaitable 超时注入统一为 `markTimeout()`**：默认 timeout policy 通过 `TimeoutMarkable` concept 检测 public `markTimeout()` 方法，无需 friend 访问。
- **统一各模块宏定义到集中头文件**：将 C/C++ 模块中分散的平台检测、编译器检测、架构检测、分支预测等宏定义提取到 `macro.h` / `macro.hpp` 文件；C 模块统一包含 `macro.h`，C++ 模块统一包含 `macro.hpp`；内核配置宏（后端选择、io_uring 能力、membarrier 检测等）集中到 `kernel_config.h`；新增 `galay_apply_cpp_module_macros` CMake 函数统一管理编译定义。
- **保留并收束 v4.9.0 之后的 owner-only 调度优化**：关闭 work-stealing 时就绪环入队使用 release store，移除无竞争路径上的 CAS 与冗余栅栏。

### Chore

- **.gitignore 新增 `.tmp*/` 临时目录排除**。
- **CTest 默认超时收紧**：`tracing_options_surface` 测试新增 300 秒超时上限。
- **benchmark 测量合同扩展**：新增 Galay/Asio TCP benchmark 不保留临时环境变量与诊断计数器的断言，并验证接收超时值保持 10ms。
- **Redis 连接池 awaitable 声明位置检查改为类定义匹配**：`hasClassDefinition()` 增加前向声明（`class X;`）过滤，避免误判。
- **io_scheduler C 模块注释与字段对齐**：ready queue 字段重命名（`tail` → `pending`），注释统一为英文。

### Fixed

- **修复 C 协程超时唤醒与移动注册回归**：超时清除等待槽位后保持任务引用直到写入最终唤醒结果；补齐 epoll pending 注册移动后的取消与关闭覆盖。
- **修复 io_uring sequence 的 onCompleted 缺失**：sequence 完成或 addSequence 失败时不再直接调用 `wakeUp()`，改走 `onCompleted()` → `cancelBoundTimeoutTimer()` → `wakeUp()` 路径，确保超时定时器在唤醒前被正确仲裁。
- **修正 timeout 竞争路径的冗余状态写入**：`completeTimeout()` 不再在 `kTimeoutWon` 时额外写入 `m_flag`，避免跨缓存行伪共享。

- **AsyncTcpSocket 新增 `readExact` / `writeAll` 组合流操作**：在一个 awaitable 状态机内完成多次部分读写，避免用户协程手动循环挂起和子 Task 分配；内部偏移量随状态机推进，零额外协程开销。同步新增 `t178_tcp_exact` 回归测试，验证部分读写、EOF 提前关闭与完整帧语义；TCP 公平吞吐 benchmark 迁移至新 API 并增加 drain 阶段 `shutdown()` 唤醒。
- **按执行语义拆分 Runtime 根任务入口**：新增 `blockOnIO()`、`blockOnCpu()`、`spawnIO()`、`spawnCpu()` 及 `RuntimeHandle` 对应入口，分别绑定 IO scheduler 或 compute scheduler；新增边界测试与提交吞吐基准。
- **新增 Boost.Asio C++ 协程 TCP/UDP 公平基线**：加入同语言 `co_spawn`/`awaitable` echo harness，与 Galay 使用相同的 100 客户端、4 worker、256 字节 payload、单请求在途 workload，并注册为 kernel 正式外部对标目标。
- **新增正式对标证据与策略门禁**：新增 TCP/UDP 三轮交替 CSV、逐轮 raw 输出及竞品策略检查，验证固定 CPU、warmup/measurement/drain、settled counter、丢包和错误字段完整性。

### Removed

- **移除含糊的默认任务提交入口**：删除 `Runtime::blockOn()`、`Runtime::spawn()` 与 `RuntimeHandle::spawn()`，避免 runtime 猜测任务类别；不可协程化同步 callable 继续使用独立的 `spawnBlocking()` 阻塞执行器。

### Changed

- **统一 readExact/writeAll 构造风格**：移除 `detail::makeExactReadAwaitable` / `detail::makeExactWriteAwaitable` 辅助函数，改为 `AsyncTcpSocket` 内直接构造 `StateMachineAwaitable`，与 `recv`/`send` 等方法保持一致；新增 public 类型别名 `ExactReadAwaitable` / `ExactWriteAwaitable` 简化返回类型。
- **迁移仓内任务提交调用点**：网络、timer 与协议任务统一使用 IO 入口，纯计算任务使用 CPU 入口，消除 ComputeScheduler 优先的默认放置语义。
- **强化 Runtime 执行器吞吐基准**：计时前启动并预热 runtime，分别采样 IO / CPU 根任务的中位数吞吐；使用完成闩锁验证 detached 根任务完成，避免逐个 `join()` 掩盖调度路径成本。
- **统一正式外部对标口径**：Boost.Asio C++ 协程是唯一竞品基线；Crossbeam、libuv、h2load、etcdctl、libpq、libmysqlclient、hiredis 和 gRPC 等入口改为历史/内部资料或 `not_applicable`，不再进入正式排名。
- **收紧网络压测测量合同**：TCP/UDP runner 采用双方严格交替的三轮采样、同一 CPU 亲和性、1 秒预热、5 秒测量、250 毫秒排空，并以 settled loss、运行时错误和关闭错误作为通过门禁；同步更新构建、性能文档和跨模块说明。
- **提高 Release 验证稳定性**：为 CTest 增加默认超时、资源锁、外部 fixture 明确开关和断言启用回归；修正 loopback 测试端口/协程等待及 sendfile、iov 边界和压力测试的确定性收敛。

### Fixed

- **修正正式压测证据归档**：TCP/UDP CSV 改为引用已提交的逐轮 raw 输出，策略测试明确拒绝被忽略的本地结果目录，并移除六份未引用且与正式样本不一致的 UDP 中间输出。

## [v4.9.0] - 2026-08-17

### Added

- **C TCP socket 补齐 `SO_REUSEPORT` 配置接口**：新增 `galay_c_tcp_socket_set_reuse_port` 及双 listener 同端口回归测试；C TCP 多线程压测服务端改为每个 I/O scheduler 独立 listener/accept 协程，使内核可在监听线程间分发连接。

### Changed

- **对齐构建安装版本**：将 CMake 项目版本与 Bazel module 版本统一更新为 `4.9.0`，使安装包元数据与当前 Git 发布版本一致。
- **C kernel ready queue 节点缓存池真正复用**：`push` 侧消费 `pop` 回收的节点（原缓存池只写不读），消除每次唤醒一次 `calloc`/`free`。
- **C kernel reactor 对齐 ET 持久注册**：注册事件统一 `EPOLLET`，掩码未变化时跳过 `epoll_ctl`，配合唤醒后乐观重试。
- **C++ 调度器本地环 owner-only 快速路径**：关闭 work-stealing 后 `pop_back` 不再发 seq_cst 仲裁栅栏、`steal_front` 直接拒绝，`setStealingEnabled` 同步传播到 ring；ready entry 唤醒延迟约降 6%。
- **精简 FILEREAD/FILEWATCH 的 Waker 拷贝**：这两个 awaitable 仅服务于 C++ 协程，`wakeUp` 不会内联销毁 controller，直接唤醒省去一次引用拷贝。

### Fixed

- **修复 epoll 合并事件的双向分发缺口**：同一 fd 的 `EPOLLIN|EPOLLOUT` 被 epoll 合并进一个事件时，one-shot 与 sequence 任一侧完成后提前返回都会丢弃另一侧就绪位；现在读写完成后均在复查 controller 有效性后继续分发另一方向，并为 send 增加持久 `EPOLLOUT` 注册（`armPersistentWrite`），配合注册前乐观写兜底。修复后 t21 双向并发 500MB 压测连续通过，`epoll_ctl` 由每事件一次 MOD 震荡（10317 次/30s）降为 6 次/全程，并新增普通 WRITE 与只读 sequence 共存回归。
- **修复 C kernel 跨线程入队唤醒缺失**：ready queue push 后事件循环仍阻塞在 `epoll_wait` 最多一个 poll 周期（10ms）；新增 eventfd 唤醒通道，跨线程入队立即唤醒 reactor，`async_waiter` 压力测试耗时从 3m33s 降至 42s。
- **修复 C++ 侧测试依赖过期与端口冲突**：t126 源码锁定改指新的纯 C `async_waiter.c` 竞态安全模式；t3/t4 默认端口由 8080 改为 28080，避让本机端口占用。

## [v4.8.1] - 2026-08-16

### Added

- **原生 C11 kernel runtime 与 bounded channel family**：C API 直接提供 runtime、stackful coroutine、reactor、TCP/UDP、文件 I/O、watcher、mutex/waiter 以及 MPMC/MPSC/SPSC bounded channel；核心 data path 不再经 `galay-c-bridge` 转发。
- **补齐多调度器 TCP 压测对照入口**：C++ TCP 客户端支持通过 `--io-schedulers` 启动多个 I/O scheduler 并按连接轮询分配；libuv TCP baseline 支持多个线程独立 event loop 与 `SO_REUSEPORT`，可在相同线程数下进行对照。

### Changed

- **HTTP/2 C API 改用仓库内置 HPACK 协议模块**：移除 nghttp2 依赖，复用 `galay::http2::HpackEncoder` / `HpackDecoder`，并让 CMake 在启用 HTTP/2 C API 时明确依赖 C++ HTTP/2 模块。
- **收敛 C ABI 文件、标识符与构建边界**：公开头和实现移除 `_c` 文件名后缀，kernel 统一采用 `galay_c_*` 标识符；CMake 改为构建原生 C11 kernel/common target，并同步迁移 HTTP、HTTP2、WS、Redis、MySQL、PostgreSQL、Mongo、etcd、MCP、RPC、SSL、tracing 和 utils 的 C API 调用点。
- **同步更新 C API 交付与性能证据**：示例、测试、benchmark、安装布局校验、使用指南和模块 README 对齐新 ABI；kernel 性能文档新增 Release 下 channel、协程、文件、timeout、UDP loopback 与 UDP 双进程压力基线，并明确双进程 TCP 和混合 timeout 的失败门禁。
- **扩展 benchmark 测量合同**：新增多调度器 TCP 客户端分配和 libuv 多 loop 实现的源码约束检查，并同步更新 C TCP 双进程 echo 的复现命令与无错误基线。
- HTTP/2 kernel 文件重命名：`frame_disp.{h,cc}` 更名为 `frame_dispacher.{h,cc}`、`out_sched.{h,cc}` 更名为 `out_scheduler.{h,cc}`；`galay-kernel/common/safetimer_mgr.hpp` 更名为 `timer_manager_mt.hpp`，同步更新 `h2_core.h`、`timer_scheduler.h`、测试、压测与文档中的 include 与路径引用。

### Removed

- **移除旧 C bridge 与无界 wrapper ABI**：删除 `galay-c-bridge`、旧 `_c` 源/头、unbounded C channel wrapper、token API 以及依赖它们的过时测试和 benchmark，避免保留兼容层。

### Fixed

- **修复 C API 协议边界与输入校验**：补齐 HPACK 静态索引、Huffman、动态表、长字段及整数溢出处理；收紧 HTTP/HTTP2/MCP header、路径、JSON、URL 和认证字段校验；WebSocket 改用随机握手 nonce 和 frame mask；修复 HTTP/2 WINDOW_UPDATE 失败回滚与 timeout benchmark accepted fd 初始化。
- **修复 C TCP accepted socket 的 scheduler 亲和交接**：`accept` 不再把新连接错误固定到 listener scheduler，改由 session coroutine 的首次 `recv/send` 绑定；新增双 I/O scheduler C coroutine 回归覆盖，避免跨 scheduler 首次 I/O 返回 `C_IOResultInvalid`。

## [v4.8.0] - 2026-08-14

### Added

- 新增 MCP `2026-07-28` 无状态 v2 协议实现，提供 `galay::mcp::v2` 的 stdio/Streamable HTTP client/server、`server/discover`、结果缓存字段、HTTP 标准 header 校验及 `x-mcp-header` 参数镜像能力。
- 新增显式 `galay::mcp::v1` 旧协议入口，并补充 v2 协议、stdio、HTTP 边界测试和吞吐 benchmark。
- **新增 MCP v2 `subscriptions/listen` 长生命周期 SSE 订阅流**：HTTP server 端通过 `notifyToolsListChanged` / `notifyResourcesListChanged` / `notifyPromptsListChanged` / `notifyResourceUpdated` 按订阅快照向显式 opt-in 的订阅连接广播通知，事件经有界通道投递；客户端新增 `listen()` 使用独立 SSE 连接接收事件并支持回调取消；协议层新增 `SubscriptionFilter`、SSE event 编解码与订阅确认/结束消息；HTTP 层新增只读响应头与增量 chunk 读取接口支撑流式消费。
- **新增订阅流回归与压测覆盖**：新增 `t18_v2_http_client_listen`（订阅生命周期与独立请求并发）、`t19_v2_stdio_client_concurrency`（stdio 客户端并发请求保护）与 `b8_v2_subscription_broadcast_pressure`（真实 SSE 订阅广播压测），并扩展 `t15_v2_protocol` / `t17_v2_http_server` 的订阅过滤、取消与 URI 精确匹配覆盖。

### Changed

- **保留原根命名空间 MCP `2024-11-05` API 行为**：更新 CMake、C++ module 导出及 MCP 文档以同时支持 v1/v2。
- **MCP 目录按协议版本归类**：将仅 v1 使用的 `client/`、`server/` 实现移入 `v1/client/`、`v1/server/`，删除 v1 散列的 `client.h`、`http_server.h`、`protocol.h`、`stdio_server.h` reexport 头；v2 参考 v1 结构拆分为 `v2/client/`、`v2/server/`，`protocol.h`、`http_headers.h` 归入 `v2/common/`；同步更新 CMake 源文件收集、C++ module 导出、测试、压测、示例与文档引用。
- **C coroutine bridge 全面无锁化**：7 个 `coro-c` bridge 移除 `std::mutex` 保护，user data 槽位改用原子 `exchange`，避免阻塞协程调度线程；`t22_coro_source_boundaries` 增加无阻塞锁源码约束，`b9_async_mutex_contended` 支持自定义迭代次数参数，并在工程准则中新增"协程与并发阻塞操作"约定。
- **C 协程直连桥接层收敛为共享 CRTP 完成状态机**：新增 `c_coro_operation_base.h`，将 tcp/udp/async-file/file-watcher 四个 bridge 各自复制的完成状态机收敛为一份 `CoroOperationBase` CRTP 基类——phase 与 finished 合并进单个原子字节，完成路径从多次 RMW 降为一次 CAS，`buildResult`/`commit`/`rollback` 改走 CRTP 静态分发消除虚调用，并新增非持有型 C 恢复 token `ResumeToken::fromNonOwningCCoroutine` 与 `perform_registered_io`/`perform_coro_close` 公共注册-等待-清理入口；桥接代码净删约 2000 行。
- **调度器 ready entry 投递去 RTTI**：`scheduleReadyEntry` 上移为 `Scheduler` 虚接口，删去 `scheduler.cc` 中按后端 `dynamic_cast` 分发；epoll/kqueue/io_uring 三后端改为 `override` 实现。
- **C 协程任务与等待请求全面无锁化**：`coro_task_c.cc` 移除 `std::mutex`/`condition_variable`，join/cancel 改用原子状态自旋等待，新增 `Cancelling` 中间态与 scheduler 线程上的立即恢复路径 `resumeTaskFromWaitImmediately`；`coro_wait_c.cc` 等待请求状态机改为原子推进，event token 内嵌进 request（不再逐次堆分配），completion 失败时回滚恢复可重试，并支持 `complete_fast_wait` 的立即恢复。
- **修复 epoll reactor 内联恢复后的悬垂访问**：`complete_one_shot` 返回是否已内联恢复，C 协程可能在 wakeUp 中就地恢复并销毁 controller，分发路径据此提前返回不再解引用旧指针；`async_tcp_c` 的立即 I/O 探测改为复用新导出的 `galay_core_coro_tcp_can_try_immediate_io`。

### Docs

- **边界约束与回归扩展**：`t22_coro_source_boundaries` 在源码扫描中加入 `std::mutex`/`condition_variable` 等阻塞同步 token 禁令并指向共享基类；`t23_coro_task` 新增 ready 态任务 cancel 场景覆盖。

## [v4.7.0] - 2026-08-12

### Changed

- **统一 C 测试文件命名规范**：将 `test/c` 下未遵循 `tN_` 前缀的测试文件按模块编号重命名，并同步调整 CMake 场景识别与测试注册。

### Added

- **补齐 PostgreSQL 与 C 模块边界回归**：新增 PostgreSQL 协议、连接池、客户端 surface 及 C ABI 边界覆盖，同时扩展 utils、WebSocket、SSL 的非法参数、截断、溢出和生命周期测试。

### Fixed

- **修复 C Base64 边界处理**：拒绝非法 padding 和非零 pad bits，并避免编码长度计算的 `size_t` 溢出。

## [v4.7.0-beta] - 2026-08-12

### Added

- **新增 PostgreSQL C 查询压测与复用查询接口**：新增真实 PostgreSQL simple query 压测入口 `benchmark_c_postgres_query_pressure`，支持 clients、queries、warmup、IO schedulers、timeout 和 SQL 配置，并输出 QPS 与 p50/p95/p99；C ABI 新增可复用 result-set 创建/重置和 `galay_postgres_client_query_into_async`，降低循环查询场景的重复分配。

### Changed

- **强化 PostgreSQL C client 协程 I/O 与生命周期契约**：复用客户端收发缓冲和 wait request，补齐 per-client 串行操作保护、错误后协议状态清理、result view 失效语义、pool lease 借用约束及公开头注释；同步补充 C/C++ PostgreSQL 性能文档和 C surface/loopback 覆盖。

### Docs

- **补充 PostgreSQL C 性能与 API 说明**：新增 C benchmark 构建入口、性能复现文档及 `postgres.h` 公开接口注释。

## [v4.6.0] - 2026-08-11

### Added

- **新增 PostgreSQL 客户端模块**：加入自研 wire protocol v3 的 C++ 同步/异步客户端，覆盖 SCRAM-SHA-256、MD5/明文认证、simple/extended query、prepared statement、事务、pipeline、连接池和显式错误传播；同时提供 C ABI、C++23 module facade、示例、测试、性能基准与 CMake/Bazel 安装入口。

- **新增 PostgreSQL 真实实例验证与高并发基准**：覆盖同步/异步查询、prepared、事务、pipeline、连接池、lease、错误后复用和 C ABI 协议边界，并提供 Galay/libpq 公平口径对照及 DataRow 解析基准。

### Changed

- **扩展构建与安装矩阵**：在 CMake presets、CMake 安装导出、Bazel、模块布局校验和 consumer smoke 中注册 PostgreSQL C++/C targets。
- **新增 TCP_NODELAY C API**：为 C TCP socket 提供显式开关，并在 PostgreSQL 异步连接路径中启用低延迟选项。
- **构建版本号升级至 4.6.0**：同步更新 CMake 包版本与 Bazel module 版本。

### Fixed

- **强化 PostgreSQL 协议错误边界与连接复用**：修复 extended query 编码错误掩盖、异步协议错误关闭、并发 connect/close 状态污染、ErrorResponse drain 和缺少 RowDescription 等问题。

### Docs

- **补齐 PostgreSQL 模块文档与构建说明**：新增 C/C++ 快速开始、架构、API、使用、示例、性能、高级主题和常见问题文档，并在模块列表、构建开关与布局校验脚本中注册 PostgreSQL。

## [v4.5.1] - 2026-08-08

### Added

- **扩展 `HttpWriter` 参数类别与所有权接口**：为 HTTP 请求和响应新增右值发送重载，TCP 路径在启动发送前将 body 转移到 writer 自有存储；响应头和请求头同时支持左值与右值调用，并在返回异步操作前保存序列化结果。
- **补齐 writer 边界回归与布局压测**：新增参数类别、`std::expected<bool, HttpError>` 返回类型、左值响应 body 保留、右值请求 body 转移和 header 序列化覆盖；HTTPS 用例扩展右值请求与左值 header 编译检查，HTTP writer 压测增加左值/右值布局场景。

### Changed

- **明确 HTTP 异步读写结果与生命周期契约**：公开 Doxygen 统一说明 `co_await` 结果为 `std::expected<bool, HttpError>`；左值响应发送保留调用方 body，右值调用显式转移所有权。
- **构建版本号升级至 4.5.1**：同步更新 `CMakeLists.txt` 与 `MODULE.bazel`，对齐 CMake 包版本与 Bazel 模块版本。

### Docs

- **精简并统一工程准则**：同步收敛 `AGENTS.md` 与 `CLAUDE.md`，明确最小端到端交付、模块边界、依赖复用、长期架构决策及不保留向后兼容的要求，同时保留 TDD、显式错误传播和返回值处理约束。

## [v4.5.0] - 2026-08-07

### Chore

- **清理本地性能产物的版本跟踪**：移除 `docs/optimization/` 与跨平台网络基准报告的 Git 跟踪，保留本地文件；忽略根目录任务报告、临时部署脚本及 Python 缓存，避免它们再次进入提交。
- **清理本地任务与部署产物**：删除根目录旧工作流计划、任务日志、状态监控及部署脚本，避免一次性操作材料继续作为项目交付物保留。
- **停止跟踪本机性能结果目录**：将 `benchmark-results/` 及其全部历史结果移出 Git 跟踪并加入忽略规则，保留本地文件供复跑和分析使用。

### Changed

- **构建版本号升级至 4.5.0**：同步更新 `CMakeLists.txt` 与 `MODULE.bazel`，对齐 CMake 包版本与 Bazel 模块版本。
- **重构 C Channel ABI 与构建入口**：C API 默认启用；公开头按 `mpmc` / `mpsc` / `spsc` 与 `bounded` / `unbounded` 组织，统一 `C_ChannelMessage`、结果码和错误字符串；无界 MPMC/MPSC 增加线程专属 producer token，MPMC 同时提供 consumer token，以避免稳态路径查询共享库 TLS 缓存。
- **优化并发队列热路径与退役块扫描**：`spsc::BoundedChannel` 在 Linux 支持 process-wide membarrier 时，以首次 waiter 注册的冷路径重屏障换取 `kReady` / `kEmpty` 的 release 发布，并在不支持的平台保留原 `seq_cst` 路径；`mpmc::UnboundedChannel` 改为逆序检查退役 block，从忙碌尾部更早结束扫描，降低回收拒绝路径的尾延迟。
- **优化 MPSC producer 独占数据面**：无界通道移除逐 slot `ready` 原子，改由 producer 通过累计 `published` tail 原子发布单条或整批消息，consumer 缓存已观察位置并按固定配额轮询 stream；bounded 通道新增固定 producer 数构造、独占 `ProducerToken` 和分片 SPSC ring，直接发送在该模式下显式返回 `kNotReady`；paired benchmark 与回归测试统一验证每 producer FIFO、checksum、关闭排空和固定容量口径。
- **扩展 MPSC paired 基准的消费模式**：C++ 与 Rust 对照程序统一支持 `single` / `batch` 消费模式，runner 增加消费模式参数、结果字段和批量上限校验，便于区分单条轮询与批量排空的吞吐口径。
- **补齐 bounded MPSC 无分配批量排空接口**：`BoundedChannel<T>` 新增 `drainTo()`，复用调用方 vector 的 spare capacity，整批推进 head 并统一探测发送 waiter；同步补充 FIFO、容量边界、并发 close 和 waiter 唤醒回归测试。
- **修复 MPMC 无界通道 token 换块瞬时失败**：token 缓存块失效时同时校验 tail position 与 tail block anchor，避免消费者回收旧块导致的短暂竞态被误报为发送失败。
- **校正并发通道基准口径与对照标识**：MPSC 吞吐、prefetch 和 C++/Rust paired runner 统一从 2P1C 起测，移除会退化为 SPSC 的 1P1C 默认场景；精简旧 MPSC 综合基准，只保留多生产者吞吐、正确性与压力覆盖，并在 MPMC Rust 输出中记录固定的 Crossbeam 实现版本。
- **模板化 RingBuffer 容量并重命名 typed SPSC 实现**：`RingBuffer` 增加编译期容量参数，固定容量默认值为 4096，动态容量必须显式使用 `std::dynamic_extent`；新增容量 concept 和 Mmap 固定容量校验。typed SPSC 实现重命名为 `type_ring_buffer.hpp`，并移除冗余容量状态。
- **优化 bounded SPSC 批处理数据面**：`Ring::split()` producer/consumer 独占本地 cursor，缓存对端 cursor，并将索引发布与回收收敛为每批一次；trivial 类型的环绕批处理使用两段 `memcpy`，减少逐元素原子访问和复制开销。
- **统一底层 Ring 读写接口**：byte `RingBuffer`、`TypeRingBuffer<T>` 及 split endpoint 统一采用 `tryWrite` / `tryRead` 与 `tryWriteBatch` / `tryReadBatch` 命名，移除旧 `write` / `read`、`trySend` / `tryRecv` 接口；Channel 层继续保留 `send` / `recv` 语义。

### Removed

- **回退未成熟的 MPSC 统一工厂 API**：删除 `channel_factory.h` 及其示例和专项测试，避免把存在已知吞吐策略线程局部 producer 生命周期问题的包装层作为稳定公开入口；调用方继续直接选择已有的具体通道类型。
- **移除旧 C Channel 平面 API**：删除 `bounded_channel_c`、`mpsc_channel_c`、`unsafe_channel_c` 与 `channel_c` 的旧头、源、测试、示例和基准，避免拓扑语义不清的兼容入口继续被安装。

### Added

- **补齐并发队列优化回归覆盖**：新增 Linux 非对称内存屏障能力、SPSC close-during-publish 与 MPMC 单次发布唤醒测试；`spsc-paired` 增加 `BoundedChannel` 内部回归 case，`b25` 增加退役 block 正序/逆序扫描尾延迟对照。
- **新增 bounded MPSC per-producer ring 模式**：`BoundedChannel(capacity, producerCount)` 将总容量静态分配到 producer 独占的 SPSC ring，通过 move-only `ProducerToken` 和 token 版 `trySend()` 消除发送热路径的共享 tail CAS；唯一 consumer 按固定配额公平轮询各 ring，旧单参数共享 ring 构造和 API 保持兼容，并补齐无效 topology、容量边界、关闭排空、异步发送拒绝与并发 FIFO 回归覆盖。
- **新增全量验证回归覆盖**：补充 io_uring `connect(EISCONN)`、benchmark 测量合同与 `CompletionLatch` 生命周期测试，并扩展 Mongo replica set 单 seed 发现、读偏好、部分 seed 故障和 setName 不匹配集成场景。
- **新增 SPSC 专用数据面与异步通道**：`galay-utils` 提供运行时容量 `SpscRingBuffer<T>` 与成员内持槽位的 `StaticSpscRingBuffer<T, N>`；`galay-kernel` 在两个公开头中提供 `Ring` / `StaticRing`、约 4 KiB 分块复用的 `UnboundedQueue`，以及支持 move-only、批量收发、调用方缓冲区与 timeout 的 `BoundedChannel<T, N>` / `UnboundedChannel<T>`。`BoundedChannel` 另提供 close/drain；动态 ring 与无界分块的构造或扩容失败通过显式错误或布尔结果返回。
- **补齐 SPSC 正确性与跨语言性能验证**：新增 ring 核心、无界跨块复用、窄游标回绕、异步 timeout 竞争、非法容量/OOM、安装消费和最终 drain 回归测试；新增严格 1P1C paired runner，以相同 `uint64_t` 序列、容量、yield 退避、FIFO/数量/checksum 门禁和 ABBA 配对样本对照 Rust Crossbeam，并输出 bootstrap 95% 置信区间、CV、原始样本与二进制哈希。
- **扩展 SPSC paired 批处理对照**：C++ 与 Rust 同步新增 `batch_unbounded` 参考 case，schema 升级到 v4，并统一输出 QPS p50/p99、CV、bootstrap 95% CI、retry ratio 及正确性、稳定性和公平性门禁；默认 CV 阈值调整为 25%。
- **新增高性能有界 MPMC 异步通道**：`galay::mpmc::BoundedChannel<T>` 基于固定容量 Vyukov ring 提供线程安全的 `trySend()` / `tryRecv()`、协程 `send()` / `recv()` / `recvBatch()`、超时、关闭唤醒和 move-only 元素支持；关闭位与 reservation cursor 共用同一 `tail` 原子，保证 close 前已取得的 reservation 完成发布并排空后才返回 `kClosed`。Apple AArch64 竞争退避使用 `isb` CPU hint，CAS miss 在当前调用内保持用户态重试。
- **新增 MPMC 无界异步通道**：`galay::mpmc::UnboundedChannel<T>` 提供显式 producer/consumer token、默认线程本地 producer 缓存、单条与批量收发、异步接收、超时及 close/drain；producer 通过 0/1 SC active publication 与 close 建立全序，接收方仅在全部 producer 静止且二次 dequeue 仍为空时返回 `kClosed`。
- **补齐 MPMC 正确性与跨语言性能验证**：新增容量边界、关闭排空、异步唤醒、timeout 竞争、move-only、producer publication 线性化及无界队列两波跨 block 复用测试；新增独立进程 paired runner，严格统一 2P2C / 4P4C、8 字节单调 payload、yield backoff、完整 checksum/drain，每组至少采集 15 对交替样本并输出 bootstrap 95% CI，在 macOS 仅接受 `perf-class-only` 线程放置。Rust 对照仅保留 Crossbeam `ArrayQueue` 与 `crossbeam-channel`，旧的单消费者结果不再参与 MPMC 结论。
- **归档并发通道原始性能证据**：新增 per-producer MPMC 本机与腾讯 Linux 的 2P2C/4P4C 对照样本，以及 SPSC v4 baseline、endpoint split、批量无界参考和最终矩阵的 CSV/JSON 结果，保留通过与未通过门禁的原始记录供后续复核。
- **新增完整 Channel family C ABI**：为 SPSC、MPSC、MPMC 分别提供有界/无界通道，支持创建/销毁、同步单条与批量收发、C coroutine 超时等待、关闭和容量状态查询；补齐 family 边界测试、安装头验证和 1P1C/4P1C/4P4C 基准。Linux 24 字节消息的 MPMC 4P4C token 路径中位吞吐为约 53.43M msg/s，较默认路径约 9.88M msg/s 提升约 5.2 倍。
- **新增 MPSC 专用有界与无界通道**：`galay::mpsc::BoundedChannel<T>` 使用多生产者 tail CAS 与单消费者 head；`galay::mpsc::UnboundedChannel<T>` 使用每 producer 独占的分块 SPSC 流、token/TLS 流缓存和高水位复用，单消费者稳态路径不做 cursor CAS/RMW；配套 close/drain、timeout、move-only、批量收发与边界/竞态测试。
- **新增 MPSC C++/Rust 成对性能验证入口**：提供同 workload 的 bounded/unbounded 1P1C–8P1C C++ 与 Crossbeam 程序、校准/交替采样 runner、FIFO/checksum/计数门禁、bootstrap 95% 置信区间与原始证据输出。

### Changed

- **统一 SPSC API 注释与跨语言性能结论口径**：`spsc` 有界/无界通道的公开函数注释对齐 `galay/kernel/async` 的 Doxygen 风格，补齐参数、返回值、并发与生命周期约束；paired benchmark 明确区分 raw/batch bounded 与非等价 unbounded 对照，保存 Linux 1P1C 原始样本、CV、bootstrap 95% CI 和门禁结果，不再以单一场景宣称全面超过 Rust。
- **优化 SPSC 无界通道轮询热路径**：缓存 producer/consumer 单调游标，polling 路径使用 release store，首次启用 waiter 后切换为 RMW 发布握手，并放宽仅由单消费者/生产者拥有的指针与 owner 清理操作的内存序。
- **校正 kernel benchmark 的生产测量口径**：scheduler 场景改为验证 Runtime 对 IO scheduler 的 round-robin 分发与双 scheduler 扩展性，明确 reactor owner-affinity 下不启用 work stealing；UDP 改为固定时长持续压流并分离 measurement window 与 settled loss；RingBuffer 增加编译器可观测 checksum/barrier，区分 mmap 逻辑环绕与 vector 物理环绕，并明确仅代表单线程热缓存内存微基准。
- **完善跨平台全量执行矩阵**：Linux/aarch64 对依赖未实现 stackful context 的 C tests/examples/benchmarks 按源码能力过滤，修正 libuv、自包含 Redis/RPC benchmark、长运行 MPSC benchmark、etcd/Redis 集成脚本和 HTTP/HTTP2 example 资源定位。
- **收敛 SPSC 热路径、存储与公开文件边界**：纯 polling ring 使用单调游标、对端游标缓存和缓存行隔离，成功稳态无 CAS/原子 RMW；无界队列移除逐消息 consumed 原子写并把跨块分配下沉为冷路径。异步 waiter/timeout 控制面与极限吞吐数据面分离，内核实现收敛为 bounded/unbounded 两个公开头，ring 基础设施统一复用 `galay-utils/cache/spsc_ring_buffer.hpp`；编译期容量 specialization 不分配槽位内存。
- **统一异步 I/O 文件与公开类型命名**：C++ 头文件和实现统一改为 `async_aio`、`async_tcp`、`async_udp`、`async_file_watcher`，公开类型改为 `AsyncAio`、`AsyncTcpSocket`、`AsyncUdpSocket`、`AsyncFileWatcher`；C wrapper 同步采用带 `_c` 后缀的新文件名，保留现有 C ABI 函数与句柄名称。
- **异步同步原语归入 async 模块**：`async_mutex` 与 `async_waiter` 从 C++ `concurrency` 和 C `concurrency-c` 目录迁入对应 `async` / `async-c` 目录，并同步更新模块入口、安装边界、源码、文档、测试、示例与 benchmark 引用。
- **收敛 `BoundedChannel` 生产实现与维护文档**：删除 `GALAY_BQ_DIAG_*` 临时编译分支，保留唯一的关闭检查、waiter-aware 和 ring 退避路径；补全公开 API、协程 awaitable、ring 与 waiter helper 的参数、返回值、错误、并发和生命周期注释。
- **优化 `BoundedChannel` C 热路径与压测口径**：成功发送不再重复读取关闭状态，批量和协程路径复用已校验的内部收发函数；吞吐 benchmark 改为完整消息计数、Release 预热/中位数采样和线程局部统计，消除共享原子与 cache-line 伪共享造成的测量偏差。
- **优化 MPMC 数据面与基准隔离能力**：无界队列改为 4096 槽可回收分段结构，使用单调 reservation cursor、块级复用和 token 局部块缓存，发送完成后的 waiter pump 外提为 unlikely 冷函数，避免同步轮询热路径内联完整控制面。B25 与独立 paired runner 可拆分当前分段队列 raw 数据面、token/waiter 通知成本和 moodycamel 默认基线；2P2C 可超过 Crossbeam，但 4P4C 仍落后，因此不保留全面胜出结论。
- **收紧 MPMC 元素异常契约**：Bounded 元素必须不可抛移动构造，Unbounded 元素必须不可抛默认构造、移动构造和移动赋值，复制发送仅对不可抛复制构造类型开放；避免元素操作异常使无界 producer 永久保持 active，或穿过 `noexcept` 完成路径终止进程。
- **Waker 恢复改用 owner scheduler 无分配入口**：`TaskState` 内嵌 resume 链接，compute/epoll/kqueue/io_uring scheduler 统一通过专用 MPSC admission 回到 owner 线程；普通注入与 resume 批次公平轮转，`stop()` 先关闭接纳再由 owner 排空，成功恢复热路径不经历堆分配。
- **统一并发通道命名空间与消费边界**：移除旧 `mpsc_channel.h` / `unsafe_channel.h` / 顶层 `bounded_channel.h`；C ABI 同步迁入 `galay::{mpmc,mpsc,spsc}` 的 topology 专属目录，HTTP/2、RPC、module facade、示例、测试、benchmark 和文档同步迁移。

### Fixed

- **消除 GCC 14 原子 shared_ptr 与 HMAC 静态诊断，并稳定网络回归测试**：HTTP/2 静态文件缓存、RPC endpoint 快照和取消回调链迁移到 `std::atomic<std::shared_ptr<T>>` 成员 API；HMAC-SHA256 改为分段哈希，避免按输入长度拼接临时缓冲区触发 `stringop-overflow`；C TCP connect timeout 固定使用 RFC 5737 文档地址，sequence duplex split 测试改为批量 drain peer buffer，避免低效逐字节读取导致的间歇超时。
- **修正 C socket 创建错误与受限环境测试语义**：TCP C API 将底层 socket 创建失败映射为 `C_TcpSocketIOFailed`；6 个依赖本地 socket 的 CTest 仅在独立探测到 `EPERM` / `EACCES` 时跳过，其他异常仍保留失败信号。

- **修复跨后端与协议正确性问题**：io_uring connect 将 `EISCONN` 视为已连接，benchmark `CompletionLatch` 在报告完成前等待最后 arrival 退出同步对象，C UDP bridge 收敛 timeout/cancel 与 IO completion 竞争；同步 Mongo 支持 replica set 发现和读偏好选择，异步 Mongo 对未支持拓扑显式报错；MySQL `caching_sha2_password` 改用协议要求的 RSA OAEP-SHA1，MurmurHash3 改用 `memcpy` 消除未对齐读取 UB。
- **修复全量测试中的返回值、并发与资源边界**：UDP benchmark、RingBuffer/readv 测试、timer 并发测试和 channel namespace 测试显式处理关键调度、socket option、计时器与 token 结果；测试 stdout 写入串行化，避免并行日志数据竞争。
- **修复 SPSC 跨块与异步完成竞态**：修复无界队列跨块越界和回收链 double-free、有界 timeout 最终检查竞态、过期 timer 立即通知误唤醒，以及 benchmark 在 producer 完成后单次空读便退出造成的潜在伪失败；consumer 现在按预期数量完成最终 drain。
- **修复全量构建中的 RPC etcd 注册变量重定义**：区分服务注册与 endpoint 注册的局部结果变量，消除两个测试/压力基准目标在同一作用域内重复声明导致的编译失败。
- **修复 channel timeout 完成竞争与提前恢复风险**：新增延迟唤醒门和事务式完成状态机，使 timeout 与破坏性 enqueue/dequeue 只产生一个完成者；完成事件在 `await_suspend` 发布结束前只记录 pending，避免协程提前恢复并销毁仍在访问的 awaiter/channel。
- **修复 HTTP/2 间歇 coredump**：epoll/kqueue reactor 改为持有稳定 registration entry，`IOController` 移动时通过反向 owner 槽原位重绑，避免晚到事件访问 moved-from `SslSocket` 中已释放的 controller；事件分发热路径不新增锁、原子、查表或分配。
- **修复 MPSC waiter/close 与发送失败处理**：vector batch waiter 在 arming 窗口改用无分配发布检查，避免分配失败遗留 arming phase；HTTP/2 出站队列、benchmark 与示例完整检查 `send` / `sendBatch` / scheduler 返回值，失败时显式关闭连接或标记压测失败。
- **修复 C++23 module 与并行测试契约**：补齐 kernel module prelude 和 `extern "C++"` 声明归属，安装导出支持 kernel BMI，include/import 示例在 GCC 14 Debug module 配置下可编译运行；CTest 对共享 TCP/UDP 测试端口加资源锁，避免 `-j2` 内部争用。

## [v4.4.2] - 2026-07-29

### Added

- **`App::version()` 支持注册版本短选项**：新增可选 `shortName` 参数，内部复用普通 `flag()` 选项注册与查找流程；调用 `version("1.2.3", 'v')` 后同时支持 `-v` / `--version`，未指定短名时继续仅注册 `--version`。
- **新增 CLI 分派压力基准**：覆盖无参数 Usage 输出与版本短选项分派，两条路径各执行 10 万次并校验返回状态。

### Changed

- **空参数调用直接显示帮助**：顶层 `App::run()` 在没有命令行实参时输出 Usage 并返回成功，不再静默执行空回调或先触发必选参数错误。
- **构建版本号升级至 4.4.2**：同步更新 `CMakeLists.txt` 与 `MODULE.bazel`。

### Fixed

- **对齐 RingBuffer 测试与显式错误契约**：零容量测试改为检查 `RingBuffer::create(0)` 返回的 `RingBufferError::kInvalidCapacity`，移除过期异常断言；默认 mmap 后端测试兼容资源创建失败后降级 vector 的一段或两段 iovec 公开语义。

## [v4.4.1] - 2026-07-28

### Changed

- **收敛 `App` 命令行执行入口**：删除仅解析参数的公开 `parseArgs()` 接口，`App` 统一通过 `run()` 负责参数解析、帮助/版本处理、错误输出与命令回调执行；该变更对直接调用 `parseArgs()` 的源码不兼容。
- **构建版本号升级至 4.4.1**：同步更新 `CMakeLists.txt` 与 `MODULE.bazel`。

### Docs

- 同步更新 utils API 参考与使用指南，移除 `parseArgs()` 及手动处理 `CliError` 的示例；相关边界测试与 module import smoke 改为验证唯一 `run()` 入口。

## [v4.4.0] - 2026-07-28

### Added

- **新增类型安全 CLI 声明与解析接口**：`App` / `Cmd` 提供 `opt<T>()`、`flag()`、`pos<T>()`、`sub()` 与 `on()`，支持默认值、必选参数、外部变量绑定、重复参数、候选集合、命名位置参数和多级子命令；数值转换与解析错误统一通过 `std::expected` 显式传播，不再依赖异常。
- **补齐现代命令行语义**：支持 `--opt=value`、`-o value`、`-ovalue`、短选项合并、`--no-flag`、`--` 终止符、帮助/版本正常终止与可重复解析；父子命令必选校验、位置参数和帮助输出顺序均由新增边界测试覆盖。

### Changed

- **拆分 CLI 公开头文件边界**：将原集中在 `app/app.hpp` 的实现拆分为 `error.hpp`、`value.hpp`、`arg.hpp`、`positional.hpp`、`cmd.hpp` 与聚合入口 `app.hpp`，并同步 module prelude 所需标准库头。
- **替换旧 CLI 公开接口**：移除 `ArgType`、`ArgValue`、`Arg`、`addArg()`、`getAs()` 等旧接口，统一迁移到编译期类型确定的 `Opt<T>` / `Positional<T>` API；该变化对直接使用旧接口的调用方不兼容。
- **构建版本号升级至 4.4.0**：同步更新 `CMakeLists.txt` 与 `MODULE.bazel`。

### Docs

- 更新 utils API 参考与使用指南，补充类型安全 CLI、手动处理 `CliError`、变量绑定和子命令解析示例。

## [v4.3.0] - 2026-07-20

### Changed

- **协程客户端 awaitable/池实现下沉 details**：galay-etcd / galay-mysql / galay-redis 三个异步客户端把 awaitable 与池 awaitable 的实现从主 `client.{h,cc}` / `conn_pool.{h,cc}` 迁入新增的 `details/awaitable.{h,inl}` 与 `details/pool_awaitable.{h,inl}`，公开头/源大幅瘦身（mysql `client.cc` 净减约 2000 行），主头文件只保留接口与类型声明。
- **HTTP/2 与 RPC 客户端 awaitable 边界统一**：将 `CaptureSchedulerAwaitable`、`H2cUpgradeAwaitable`、`RecvRpcResponseChainAwaitable` 及 RPC 响应读取状态迁入各模块 `details/*_awaitable.{h,inl}`；公开 client 头仅保留前置声明、接口与 details include，并保留任意 `Strategy` / `SocketType` 模板实例化能力。
- **EtcdClusterClient 重构为无锁 EtcdClient 池**：移除原 cluster wrapper 的 `put/get/del/grantLease/keepAliveOnce/pipeline` 等逐请求方法与内置重试/健康探测循环，改为为每个 endpoint 预建固定数量 `EtcdClient`、通过 `tryAcquire()` 返回 move-only RAII 租约 `EtcdClientLease` 的无锁空闲队列模型，并提供 `acquireConnected()` / `withClient()` 高阶入口；租约析构或 `release()` 归还连接，池空返回新增 `EtcdErrorType::PoolExhausted`。
- **galay-mcp 命名空间统一为 C++17 形式**：15 个 mcp 头/源由旧式嵌套 `namespace galay { namespace mcp {` 改为 `namespace galay::mcp {`，不改任何符号名。
- **池 state 缓存行对齐**：etcd cluster 池状态字段与 redis 连接池 `IdleShard` 按 `alignas(64)` 对齐，降低多线程取还路径的伪共享。

### Added

- **cluster 连接复用配置与池便捷 API**：`EtcdProductionConfig` 新增 `connections_per_endpoint` 字段，`EtcdClusterClientBuilder` 新增 `connectionsPerEndpoint()`；mysql / redis 连接池补齐 `LeaseAwaitable` 等高阶便捷 awaitable。
- **新增 cluster 连接复用 benchmark / 测试 / 示例**：`b6_cluster_connection_reuse`、`t18_cluster_connection_reuse`、`t19_pool_convenience` 与 `e3_client_pool`（import + include 两种形式）覆盖无锁池取还吞吐、池空返回 `PoolExhausted`、租约析构归还、多线程并发安全与池 move 后仍可用等契约。

### Fixed

- **修复 h2c 客户端测试的 listener 启动竞态与错误漏检**：`H2cServer` 新增无锁 `isReady()`，仅在 listener 完成 bind/listen 后报告就绪；`http2.h2ccl` 改为有界等待可观测 ready 状态，并完整处理 connect/shutdown 的 Task 外层与业务内层 `std::expected` 错误。

### Docs

- **更新 etcd 模块文档**：同步架构设计、API 参考、使用指南、示例代码、性能测试与常见问题；新增 `docs/cpp/modules/etcd/refactor-plan.md` 重构执行文档。

## [v4.2.1] - 2026-07-15

### Added

- **新增 Debug 构建开关**：`cmake/option.cmake` 增加 `GALAY_BUILD_DEBUG`，默认关闭；单配置生成器在未显式选择其他非 Debug 构建类型时默认使用 `Release`，开启后切换为 `Debug`。多配置生成器同步设置默认构建配置，并保留通过 `--config` 显式选择配置的标准行为。

### Changed

- **对齐 preset 与配置测试**：`developer-full`、`consumer-minimal` 和 `linux-perf` 通过 `GALAY_BUILD_DEBUG=ON` 保持开发期 Debug 语义，`linux-perf-release` 显式关闭该开关并继续保留 `RelWithDebInfo` 与 frame pointer 配置；新增 `config.build_type_option`，覆盖默认 Release、显式 Debug 与显式 `RelWithDebInfo` 三条配置路径，tracing 配置测试同步改用新开关。

## [v4.2.0] - 2026-07-14

### Added

- **新增 kqueue READ 常驻与注册 batch 边界测试**：增加 `kernel.kqueue_persistent_read` 与 `kernel.kqueue_registration_batch_source`，覆盖 recv/readv 完成后保留 READ 兴趣、send/writev 完成后解除 WRITE 兴趣，以及简单 awaitable 变更进入 pending batch 的源码约束。
- **新增同 wire 的 libuv TCP/UDP echo 基线**：增加可选 `benchmark_c_kernel_libuv_echo_server`，通过 pkg-config 探测 libuv，并以单事件循环、相同 Galay client/wire workload 输出连接、吞吐与错误计数，支持在 kqueue / epoll 上进行可审计的同后端探针。
- **归档 2026-07-12 跨协议 fresh benchmark 证据**：新增 kernel、HTTP、HTTP2、WS、SSL、MCP 与 RPC 的 CSV/TXT 原始记录，明确编译器、后端、协议形态、正确性门禁、blocked 原因及“仅基线/不参与排名”等状态。
- **新增 RPC 服务器错误边界测试与注册压力 benchmark**：覆盖无 `shared_ptr` 注册表面、重复/容量耗尽错误、bind 失败同步返回及 stopped 状态；压力基准对 unary/stream 注册执行 128 万次验证，成功路径保持零堆分配。
- **io_uring 支持 UDP multishot recvmsg**：`IOUringReactor` 新增基于 `IORING_OP_RECVMSG_MULTISHOT` + provided buffer ring 的 UDP 数据报接收路径，运行时探测内核（≥6.0）与 liburing 能力，能力不足时回退兼容 one-shot recvmsg；新增独立 UDP buffer group（`kRecvFromBufferGroup=1`，单缓冲覆盖最大 UDP payload 与 recvmsg 元数据），并通过 `m_recvmsg_multishot_confirmed` 区分真正的能力缺失与运行期 `EINVAL`。
- **epoll 支持持久 READ 兴趣**：`EpollReactor` 为 recv/readv 引入 `armPersistentRead`，跨 awaitable 持久保留 `EPOLLIN`，配合注册前非阻塞乐观读消除重复 `epoll_ctl`；WS 固定口径下 `epoll_ctl` 由 7,022 降至 36，吞吐提升 3.86%。
- **新增 UDP multishot 与 epoll 持久读回归测试**：`kernel.iouudp_multishot(_src)` 覆盖能力门控、recvmsg 元数据解析、one-shot 回退、截断丢余量与多源端口回填；`kernel.epoll_persistent_read(_source)` 覆盖 recv/readv 完成后保留 READ 兴趣与解除语义。
- **新增跨协议竞品基准对照目标与脚本**：benchmark 通过 pkg-config 探测并构建 `libmysqlclient` / `hiredis` 对照压测目标；新增 etcd/etcdctl、MySQL/libmysqlclient+mysqlslap、Redis/hiredis+官方 redis-benchmark+连接池自对照、以及 UDP/WS 传输（libuv / libwebsockets）固定口径对比脚本。

### Changed

- **优化 kqueue 稳态注册与多 fd 批量提交**：`IOController` 新增简单槽位 armed mask，recv/readv 的 `EVFILT_READ` 完成后常驻，send/writev 的 `EVFILT_WRITE` 仍按需 arm/disarm；简单 awaitable 的 kevent 变更进入 `m_pending_changes`，在 poll 边界或 32 项阈值统一提交，并与 sequence 兴趣位取并集避免误删。macOS 32 连接 / 1024B / 5s 中位吞吐由 `156,267 QPS` 提升至 `172,594 QPS`，约提升 `10.45%`。
- **收敛 benchmark 公平性与可复现参数**：MCP benchmark 支持请求数参数并改用单调时钟；kernel TCP client 移除事件循环线程上的阻塞起跑门，UDP server 支持端口参数；SSL 吞吐与 steady-state 场景固定 TLS 1.3 AES-128-GCM ciphersuite并处理配置失败。
- **明确 HTTP2/RPC 竞品对照边界**：h2load 脚本按工具能力启用 histogram，缺失时显式标记 percentile unavailable，并将 nghttpd echo 降级为互操作参考而非排名对象；RPC 脚本支持外部 build/result 目录，区分 network 与 in-process 场景，仅允许相同自定义 wire 的 fixture 参与排名，gRPC 仅作为不同协议 sidecar 状态记录。
- **Redis 异步客户端吞吐计时改用双精度毫秒**：`b1_async_client_throughput` 的压测时长由整型毫秒截断改为 `duration<double, std::milli>`，避免短样本 QPS 的量化误差。

### Fixed

- **修复 Apple libc++ 的 atomic shared_ptr 构建兼容与 kqueue 清理错误可观测性**：RPC cancellation 回调链、endpoint 快照与 HTTP/2 静态文件 body cache 由 `std::atomic<std::shared_ptr<T>>` 改为普通 `shared_ptr` + shared_ptr 原子自由函数，保留并发读取期间的引用计数生命周期；kqueue 路径同步处理 `kevent` / `close` 结果，并在 remove/close 时丢弃尚未提交的变更。
- **RPC 服务器注册与启动改为显式错误传播**：`RpcServer` / `RpcStreamServer` 使用借用的 `RpcService&` 和固定 64 槽开放寻址表，移除注册所需的 `shared_ptr` 控制块与容器分配；注册返回 `std::expected<void, RpcError>`，显式报告空名称、重复、启动后注册和容量耗尽。
- **RPC 启动成功语义对齐监听就绪**：`start()` 改为 `std::expected<void, RpcError>`，依次检查 runtime、socket、选项、bind、listen 与 accept-loop 调度，只有监听完成后才设置 running；同步更新 unary/stream 测试、benchmark、include/import 示例及 API 文档以处理所有返回值。

### Docs

- **补齐跨平台与全模块竞品性能文档**：新增 `docs/cpp/cross_platform_network_benchmark_2026-07-13.md` 跨平台 C/C++ 网络竞品对比，更新 etcd/http2/kernel/mongo/mysql/redis/rpc/ssl/tracing/ws 各模块性能测试文档，归档 UDP multishot 优化、epoll 持久读与各竞品对照的原始 CSV、图表与 raw 数据；`docs/machine_config.md` 追加 2026-07-13 全模块对比执行快照。
- **记录 WS 竞品依赖**：跨平台 WebSocket 明文 echo 竞品基准使用 libwebsockets 4.5.8；源码包不再随仓库分发，复现时请从官方源码获取并校验 SHA-256 `b6ade658f4af3a823d0dc806ae5ef0623f0f4f5e2aeb895a0f77c4783840c30e`。

## [v4.1.0] - 2026-07-10

### Changed

- **Linux 性能构建与同机压测口径落地**：新增 `linux-perf-release` CMake preset，使用 `RelWithDebInfo`、`-O2` 与 frame pointer 构建全模块 benchmark；在 Tencent 4C4G Linux 上补跑 clean `HEAD` baseline 与当前快照 5 轮同 workload 压测，记录 HTTP static、HTTP2 static、route match、writer layout、sendfile、utils、Redis pool 与 RPC cancel notify 的可比结果，并确认 current 构建 warning 为 0。
- **HTTP/1 与 HTTP/2 静态文件链路异步化**：HTTP MEMORY 静态文件读取改为 blocking executor + `AsyncWaiter` 挂起等待，HTTP server root task 显式绑定 runtime；HTTP/2 静态文件 cache 提升为 server 级共享缓存，cache miss 通过 blocking worker 读取并回投 frame，避免事件循环同步读文件。
- **Redis pool 与 RPC 取消通知热路径优化**：Redis async pool 收敛 acquire/release 并发状态、等待队列与计数器更新，减少跨线程竞争；RPC pending cancel 从每 call watcher/扫描改为 cancellation token callback 直接通知 pending waiter，降低取消路径资源放大。
- **HTTP route / writer 与 utils 热路径分配优化**：HTTP 路由匹配改用 `string_view` 段扫描和小型连续 route params，兼容 `routeParams()` 懒加载 map；HTTP writer TCP layout 复用固定 iovec cursor，SSL 合并路径复用成员 buffer；LRU 热 key 访问改为惰性刷新过期节点，ConsistentHash 原子快照读写改用更弱但足够的 acquire/release/relaxed 内存序。
- **SSL 引擎 BIO 接口改为 std::expected 显式错误传播**：`SslEngine::feedEncryptedInput` / `extractEncryptedOutput` 由返回 `int`（-1 表示错误）改为 `std::expected<size_t, SslError>`，新增 `SslErrorCode::kBufferTooLarge` 表达缓冲区超过 `INT_MAX` 的失败原因，并经 `SslError::fromOpenSSL` 保留 OpenSSL 错误链；C 包装 `ssl.cc` 与 C++ awaitable 同步消费新的 expected 返回，移除原先的哨兵 `int` 错误判断（遵循错误必须经返回值显式传播的约定）。
- **SSL 状态机错误映射收敛为 `mapSslError` 约定并加编译期约束**：`SslStateMachineAwaitable` 新增 `ssl_error_expected` / `ssl_error_mappable_expected` 类型 trait 与 `HasSslErrorMapper` concept，通过 `static_assert` 强制 `result_type` 必须是可直接承载 `SslError`、或经 `mapSslError` 映射的 `std::expected`；新增 `makeUnexpected()` 集中错误构造，移除原先命中不可表达错误时的 `std::abort()` 兜底，统一返回 `kUnknown`。HTTP / HTTP2 / Redis / WS 各 SSL 状态机补齐 `mapSslError(SslError)` 映射入口。
- **RESP double 解析改 `strtod` 去异常**：`redis_protocol.cc` 的 double 帧解析由 `std::stod`（throw）改为 `std::strtod` + errno / 长度 / 溢出校验（限制文本长度上限），超范围或格式非法时显式返回 `ParseError::InvalidFormat`，移除协议解析路径上的异常控制流。
- **mvcc / Transaction 禁用拷贝与移动**：`VersionedValue` 显式 default move、delete copy；`Mvcc` / `Transaction` 因持有 `shared_mutex` / 引用而禁用拷贝与移动，消除隐式误拷贝风险，延续对象所有权契约收敛。
- **C++ 对象所有权契约收敛为 move-only + 显式 clone**：覆盖 kernel / utils / HTTP / HTTP2 / WS / RPC / Redis / MySQL / Mongo / Etcd / MCP / SSL / tracing 等模块，非平凡状态对象禁用隐式拷贝，保留显式移动，并通过 `clone()` 暴露可审计的深拷贝入口；借用 payload / buffer 视图在 clone 时物化为独立自有存储，避免隐藏浅拷贝和生命周期悬空。
- **RingBuffer 模板化支持后端策略选择**：`RingBuffer` 新增 `RingBufferBackendStrategy::{Mmap, Vector, Auto}` 模板参数，默认使用 `Mmap` 后端以提供跨环绕边界的单段连续 span/iovec 视图；`Vector` 保留原有双段环绕行为，`Auto` 按容量选择后端。
- **协议客户端传播 RingBuffer 后端策略**：Redis / MySQL / RPC / HTTP2 / HTTP / WS / Mongo 等持有 RingBuffer 的连接、客户端、awaitable 与测试/示例/benchmark 类型补齐模板参数，默认保持 `RingBufferBackendStrategy::Mmap`，并保留显式 `Vector` 实例化覆盖。
- **全模块结构体字段重排以优化内存布局**：对 kernel / http / http2 / ws / rpc / mcp / redis / mysql / mongo / etcd / ssl / tracing / utils 及全部 C ABI 模块（`src/c/galay-*-c`）中的 struct / class 成员按访问热点与尺寸重排，把分散的小尺寸标量、指针与 bool 收敛到更紧凑的位置以减少 padding、提升缓存命中率；同步更新聚合初始化 `{...}` 顺序与构造初始化列表顺序以匹配新声明顺序（消除 `-Wreorder`）。
- **TCP 完成状态位压缩为位域**：`c_coro_tcp_bridge.cc` 的 `CoroTcpOperationBase` 用单个 `uint64_t m_flags` 位标记替代 `m_finished` / `m_complete_accepted` 两个独立 bool，并提供 `finished()` / `completeAccepted()` / `setFinished()` / `setCompleteAccepted()` 访问器，进一步压缩对象尺寸。
- kernel IO 上下文中部分 bool / 枚举状态字段统一改为 `uint64_t` 承载：`ReadvIOContext` / `WritevIOContext` 的 `m_immediate_result`、`FileWatchIOContext` 的 `m_events`、`SequenceAwaitableBase` 的 `m_registered`。

### Added

- **C ABI 补齐 `*_get_error` 错误字符串入口**：为 `galay_etcd` / `galay_rpc` / `galay_coro_ioresult` 错误码新增 `*_get_error()` 约定函数（语义与既有 `*_error_string` 完全一致，覆盖全部枚举值），满足「每个公开 C 错误码枚举都需配套错误字符串获取函数」要求；补齐 `test/c/{etcd,kernel,rpc}` 下 `t6_error_get_error.c` / `t28_coro_ioresult_get_error.c` / `t4_error_get_error.c` 白盒测试。
- **RingBuffer 新增 expected 工厂 `create()`**：`RingBuffer::create(capacity)` 以 `std::expected<RingBuffer, RingBufferError>` 表达容量非法（`kInvalidCapacity`）等可恢复失败，避免在工厂路径用异常表达容量校验失败。
- **新增 SSL 引擎 BIO 边界测试与 benchmark**：`test/cpp/ssl/t15_engine_bio_boundaries.cc` 与 `benchmark/cpp/ssl/b5_engine_bio_boundaries.cc` 覆盖 expected 化后的 BIO 读写边界与空 `wbio` / `rbio` 失败路径、超大缓冲区 `kBufferTooLarge` 分支。
- **benchmark 补 RESP double 解析场景**：`b7_resp_parser_throughput` 新增 double 帧解析吞吐场景与 payload 校验。
- **新增 ownership surface 测试与 clone/move 压力基准**：在 `test/cpp/{etcd,http,http2,kernel,mcp,mongo,mysql,redis,rpc,ssl,tracing,utils,ws}` 下新增 move-only / clone 契约测试，覆盖类型 trait、深拷贝独立性、移动后视图重绑定与注册项所有权；在对应 `benchmark/cpp/*` 模块新增 clone/move ownership pressure 基准，锁定迁移后的复制成本和移动路径行为。
- **utils 新增跨平台进程优先级接口**：`Process` 新增 `priority()` / `setPriority()` 静态方法，POSIX 平台基于 `getpriority` / `setpriority`（nice 值 `[-20,19]`），Windows 平台映射到 priority class；新增 `ProcessPriorityError` 错误枚举与配套的 `processPriorityErrorString()` 错误描述函数，错误经 `std::expected<T, ProcessPriorityError>` 显式传播，errno / `GetLastError` 立即转换为具体错误码（遵循错误显式传播与每个错误码配套错误字符串的约定）。`module_prelude.hpp` 补齐 `<cerrno>` / `<sys/resource.h>` 头，并新增 `test/cpp/utils/t11_platform_process_system.cc` 白盒测试。
- **utils Process 新增 CPU 核心亲和性接口**：`Process` 新增 `cpuAffinity()` / `setCpuAffinity()` 静态方法，Linux 基于 `sched_getaffinity` / `sched_setaffinity`，Windows 基于 process affinity mask，其他平台显式返回 `ProcessAffinityError::Unsupported`；新增 `processAffinityErrorString()`，并补齐边界测试、roundtrip benchmark 与 API/使用文档。

### Fixed

- **修复 Linux io_uring 与 sendfile 进度回归**：Linux io_uring 构建下 `sigpipe_policy` / `sendfile_one_shot_progress` 测试按真实 CQE 入口校验；`SendFileIOContext` 短发送后推进 offset/count 并累计结果，当前 one-shot 8MiB x 8 压测 5 轮均完成，clean `HEAD` baseline 在同 workload 下稳定停在 `65536/8388608`。
- **修复 epoll AIO 与 file watcher 事件处理边界**：AIO completion 收割改为非阻塞处理，file watcher 读取路径循环解析缓冲区内全部 `inotify_event` 并 drain 至 `EAGAIN`，补齐 burst 文件事件和非阻塞源码边界测试。
- **消除 Linux GCC 14 构建 warning**：`RpcEndpointCache` 改用 `std::atomic<std::shared_ptr<...>>` 成员 `load/store`，替换 deprecated shared_ptr atomic free function；MCP HTTP 示例移除 coroutine frame 中无 linkage 本地 lambda 捕获，current 完整增量构建 warning grep 为 0。
- **修复 etcd HTTP 头 token 解析误命中**：`parseHttpHeaders` 的 `transfer-encoding: chunked` 与 `connection: close` 判定由裸子串匹配改为按逗号分词的 `containsAsciiTokenIgnoreCase`，避免 `keep-alive` 等相邻 token 或噪声子串导致的误判；async / sync 两条解析路径同步对齐。
- 修复 move-only 契约迁移后的 C API wrapper、示例与 benchmark 编译边界：Mongo C wrapper 对嵌套 document / array / reply 改为显式 `clone()`，tracing C wrapper 改用 `LogRecord` 构造函数，HTTP / RPC / WS / tracing 示例和吞吐 benchmark 改为移动取值或引用绑定，避免隐式拷贝 move-only 类型。
- 修复 RingBuffer 模板化重构后的回归测试期望：HTTP recv window 与 MySQL multi-result source 测试对齐默认 `Mmap` 单段视图和模板化类型名；C coroutine ResumeToken 测试移除违反 no-exception 契约的抛异常入口用例。
- 恢复 Linux examples/benchmarks 执行矩阵的稳定入口：新增 `scripts/verify_linux_exec_matrix.py` 兼容 shim，转发并重导出 `scripts/common/105_verify_linux_exec_matrix.py`，保持现有脚本测试可 import 旧路径。

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
- **统一 utils C ABI 状态码到 `galay_status_t`**：`galay_utils_status_t` 改为 `galay_status_t` 别名，`GALAY_UTILS_*` 宏映射到 `GALAY_*`；utils 全部导出函数签名统一返回 `galay_status_t` 并标注 `GALAY_C_API`，`utils.h` 改用 `GALAY_C_BEGIN_DECLS`；`test/c/utils/header_smoke.c` 增加 `_Static_assert` 锁定新签名，`galay-c-utils` 显式链接 `galay::c-common`。
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
