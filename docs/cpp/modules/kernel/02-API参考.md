# 02-API参考

本页只记录当前公开头文件、导出 target 与真实构建选项中存在的接口；更高层工作流见 `docs/03-使用指南.md`，真实示例清单见 `docs/04-示例代码.md`。

## 导出 target

- `galay-kernel`
- `galay::kernel`
  - C++23 module file set 直接挂载在该 canonical target 上
  - 仅在 `ENABLE_CPP23_MODULES=ON`
  - 且 `GALAY_KERNEL_CPP23_MODULES_EFFECTIVE=TRUE`
  - 安装后仍通过 `galay::kernel` 消费

## 构建选项

- `BUILD_TESTING`（CTest 标准选项；默认 `OFF`）
- `BUILD_BENCHMARKS`
- `BUILD_EXAMPLES`
- `ENABLE_CPP23_MODULES`
- `BUILD_SHARED_LIBS`
- `DISABLE_IOURING`

当前仓库没有 `ENABLE_LOG` 选项，也没有 `spdlog` 依赖链路。

当前不支持 Windogalay-ws/IOCP 后端；Windows 配置阶段会直接失败并提示 `Windogalay-ws/IOCP backend not yet implemented`。

## 安装包头文件边界

- 当前包配置文件会导出 `GALAY_KERNEL_VERSION`、`GALAY_KERNEL_BACKEND`、`GALAY_KERNEL_INCLUDE_DIR`
- 若 `concurrentqueue` 头文件不在标准系统前缀，consumer 需要额外传入 `GALAY_KERNEL_CONCURRENTQUEUE_INCLUDE_DIR`
- 稳定 direct-include 入口以本页“稳定公开头快速索引”和实际安装树中的公开头为准
- 当前源码尚未额外导出 `GALAY_KERNEL_SUPPORTED_HEADERS` / `GALAY_KERNEL_INTERNAL_HEADERS` 这类头文件边界变量
- `awaitable.h`、`io_controller.hpp`、`timeout.hpp`、`scheduler.hpp`、`io_scheduler.hpp` 默认仍属于低层扩展 / 排障入口；日常业务优先使用 `Runtime`、`TcpSocket`、`UdpSocket` 等高层接口
- 其中 `awaitable.h` 在 `v3.2.0` 起承载正式的组合式扩展面：`SequenceAwaitable`、`SequenceStep`、`AwaitableBuilder`、`ParseStatus`
- `v3.3.0` 进一步补充 `AwaitContext`、`SequenceOwnerDomain` 与共享 state-machine `.timeout(...)` 收口，便于 builder / direct awaitable 统一扩展
- 当前工作树未包含受版本控制的 package consumer fixture；如需验证安装消费，请单独准备最小 consumer 工程

## 稳定公开头快速索引

按模块归类的公开安装面：

- Runtime / 调度：
  - `galay-kernel/core/runtime.h`
  - `galay-kernel/core/task.h`
  - `galay-kernel/core/scheduler.hpp`
  - `galay-kernel/core/io_scheduler.hpp`
  - `galay-kernel/core/compute_scheduler.h`
  - `galay-kernel/core/epoll_scheduler.h`
  - `galay-kernel/core/kqueue_scheduler.h`
  - `galay-kernel/core/uring_scheduler.h`
  - `galay-kernel/core/timer_scheduler.h`
  - `galay-kernel/core/awaitable.h`（仅当你要做底层组合 Awaitable / 协议扩展）
- 网络与文件：
  - `galay-kernel/async/tcp_socket.h`
  - `galay-kernel/async/udp_socket.h`
  - `galay-kernel/async/async_file.h`
  - `galay-kernel/async/aio_file.h`
  - `galay-kernel/async/file_watcher.h`
- 通用辅助：
  - `galay-kernel/common/handle_option.h`
  - `galay-kernel/common/host.hpp`
  - `galay-kernel/common/sleep.hpp`
  - `galay-kernel/common/buffer.h`
  - `galay-kernel/common/file_descriptor.h`
  - `galay-utils/cache/byte_queue_view.hpp`
  - `galay-kernel/common/error.h`
- 并发：
  - `galay-kernel/concurrency/async_mutex.h`
  - `galay-kernel/concurrency/async_waiter.h`
  - `galay-kernel/concurrency/mpsc_channel.h`
  - `galay-kernel/concurrency/unsafe_channel.h`
- 模块：
  - `galay-kernel/module/module_prelude.hpp`
  - `galay-kernel/module/galay_kernel.cppm`

## 命名空间与头文件范围

- `galay::kernel`
  - `Runtime` / `RuntimeBuilder` / `RuntimeConfig`
  - `Task<T>` / `JoinHandle<T>` / `TaskRef`
  - `ComputeScheduler` / `IOScheduler`
  - `sleep(...)`
  - `TimerScheduler`
  - `HandleOption`
  - `AsyncMutex` / `MpscChannel<T>` / `UnsafeChannel<T>` / `AsyncWaiter<T>`
  - `FileWatchEvent` / `FileWatchResult`
- `galay::async`
  - `TcpSocket`
  - `UdpSocket`
  - `AsyncFile`
  - `AioFile`
  - `FileWatcher`

## 模块门面 `galay.kernel`

头文件 / 模块入口：

- `galay-kernel/module/module_prelude.hpp`
- `galay-kernel/module/galay_kernel.cppm`

模块生效条件：

- `ENABLE_CPP23_MODULES=ON`
- `GALAY_KERNEL_CPP23_MODULES_EFFECTIVE=TRUE`
- 当前 AppleClang / 不支持模块的生成器下，源码文件会保留，但不会生成可消费的 import target

`import galay.kernel;` 当前导出面：

- 通用类型：`defn.hpp`、`error.h`、`host.hpp`、`handle_option.h`、`buffer.h`、`sleep.hpp`
- Runtime：`task.h`、`scheduler.hpp`、`io_scheduler.hpp`、`compute_scheduler.h`、`runtime.h`、`timer_scheduler.h`
- 并发：`mpsc_channel.h`、`unsafe_channel.h`、`async_mutex.h`、`async_waiter.h`
- IO：`tcp_socket.h`、`udp_socket.h`、`file_watcher.h`
- 平台裁剪：
  - `async_file.h` 仅在 `USE_KQUEUE` 或 `USE_IOURING`
  - `aio_file.h` 仅在 `USE_EPOLL`

注意：

- `galay.kernel` 是聚合导出入口，适合模块消费；direct-include 仍以本页列出的稳定公开头与实际安装树内容为准
- `scheduler.hpp`、`io_scheduler.hpp` 会进入模块可见面，但当前仍不应把它们当成稳定的 direct-include 业务入口头

## 地址、错误与字节缓冲工具

头文件：

- `galay-kernel/common/host.hpp`
- `galay-kernel/common/error.h`
- `galay-utils/cache/bytes.hpp`
- `galay-kernel/common/buffer.h`

`IPType` / `Host`：

- `enum class IPType : uint8_t { IPV4, IPV6 }`
- `Host()`
- `Host(IPType proto, const std::string& ip, uint16_t port)`
- `Host(const sockaddr_in& addr)`
- `Host(const sockaddr_in6& addr)`
- `static Host fromSockAddr(const sockaddr_storage& addr)`
- `bool isIPv4() const`
- `bool isIPv6() const`
- `std::string ip() const`
- `uint16_t port() const`
- `sockaddr* sockAddr()`
- `const sockaddr* sockAddr() const`
- `socklen_t* addrLen()`
- `socklen_t addrLen() const`

语义说明：

- `Host()` 默认构造一个清零后的 IPv4 地址槽位：`ss_family=AF_INET`，`addrLen()==sizeof(sockaddr_in)`
- `Host(IPType, ip, port)` 直接调用 `inet_pton(...)` 写入底层地址，但当前构造函数本身不返回解析错误；文本地址是否合法，最终由后续系统调用通过 `IOError` 暴露
- `fromSockAddr(...)` 会按 `ss_family` 自动选择 `sockaddr_in` / `sockaddr_in6` 的有效长度

`IOErrorCode` / `IOError` / `Infallible`：

- `enum IOErrorCode : uint32_t`
  - `kDisconnectError`
  - `kNotReady`
  - `kParamInvalid`
  - `kRecvFailed`
  - `kSendFailed`
  - `kAcceptFailed`
  - `kConnectFailed`
  - `kBindFailed`
  - `kListenFailed`
  - `kOpenFailed`
  - `kReadFailed`
  - `kWriteFailed`
  - `kStatFailed`
  - `kSyncFailed`
  - `kSeekFailed`
  - `kTimeout`
  - `kNotRunningOnIOScheduler`
- `static bool IOError::contains(uint64_t error, IOErrorCode code)`
- `IOError(IOErrorCode io_error_code, uint32_t system_code)`
- `uint64_t code() const`
- `std::string message() const`
- `void reset()`
- `class Infallible`

语义说明：

- `IOError::code()` 把系统错误码放在高 32 位、`IOErrorCode` 放在低 32 位
- `IOError::contains(...)` 只检查组合码里的低 32 位逻辑错误码
- `IOError::message()` 会拼接逻辑错误文案与 `strerror(system_code)`；当 `system_code==0` 时文本尾部会是 `(sys: no error)`
- `Infallible` 是空标记类型，用在逻辑上不会失败的路径或模板特化里

`Bytes`：

- `Bytes()`
- `Bytes(std::string& str)`
- `Bytes(std::string&& str)`
- `Bytes(const char* str)`
- `Bytes(const uint8_t* str)`
- `Bytes(const char* str, size_t length)`
- `Bytes(const uint8_t* str, size_t length)`
- `Bytes(size_t capacity)`
- `Bytes(Bytes&& other) noexcept`
- `Bytes& operator=(Bytes&& other) noexcept`
- `static Bytes fromString(std::string& str)`
- `static Bytes fromString(const std::string_view& str)`
- `static Bytes fromCString(const char* str, size_t length, size_t capacity)`
- `const uint8_t* data() const noexcept`
- `const char* c_str() const noexcept`
- `size_t size() const noexcept`
- `size_t capacity() const noexcept`
- `bool empty() const noexcept`
- `void clear() noexcept`
- `std::string toString() const`
- `std::string_view toStringView() const`
- `bool operator==(const Bytes& other) const`
- `bool operator!=(const Bytes& other) const`

语义说明：

- 构造函数族会分配并深拷贝数据，`Bytes` 本身是 move-only；没有公开拷贝构造 / 拷贝赋值
- `fromString(std::string&)`、`fromString(std::string_view)`、`fromCString(...)` 生成的是 non-owning 视图，底层存储必须由调用方继续持有
- `clear()` 会释放 owning 存储并把对象重置为空视图
- `toString()` 返回副本；`toStringView()` 返回零拷贝视图
- `c_str()` 当前实现会在末尾不是 `\0` 时尝试原地补终止符，因此更适合本身可写、并且为终止符预留了容量的文本缓冲；对二进制数据或精确容量缓冲，更稳妥的读取方式是 `toString()` / `toStringView()`

`ByteMetaData` 与辅助函数：

- `struct ByteMetaData { uint8_t* data; size_t size; size_t capacity; }`
- `ByteMetaData(std::string& str)` / `ByteMetaData(std::string_view str)`
- `ByteMetaData(const char* str)` / `ByteMetaData(const uint8_t* str)`
- `ByteMetaData(const char* str, size_t length)` / `ByteMetaData(const uint8_t* str, size_t length)`
- `ByteMetaData mallocBytes(size_t length)`
- `ByteMetaData deepCopyBytes(const ByteMetaData& meta)`
- `void reallocBytes(ByteMetaData& meta, size_t length)`
- `void clearBytes(ByteMetaData& meta)`
- `void freeBytes(ByteMetaData& meta)`

语义说明：

- `Bytes`、`ByteMetaData` 与辅助函数由 `galay-utils/cache/bytes.hpp` 提供，`galay-kernel` 不再保留本地 `bytes.h` / `bytes.cc`
- `ByteMetaData` 是公开可见但偏底层的原始缓冲描述结构；业务代码更推荐优先使用 `Bytes` / `Buffer`
- `mallocBytes(...)` 分配容量并把 `size` 初始化为 `0`
- `deepCopyBytes(...)` 会按源对象的 `capacity` 分配并复制已有 `size`
- `reallocBytes(...)` 在缩容时会同步截断 `size`；若重新分配失败会抛 `std::bad_alloc`
- `clearBytes(...)` 只清空内容并保留容量；`freeBytes(...)` 才真正释放内存

`Buffer` / `RingBuffer`：

- `Buffer()`
- `Buffer(size_t capacity)`
- `Buffer(const void* data, size_t size)`
- `Buffer(const std::string& str)`
- `void clear()`
- `char* data()`
- `const char* data() const`
- `size_t length() const`
- `size_t capacity() const`
- `void resize(size_t capacity)`
- `std::string toString() const`
- `std::string_view toStringView() const`
- `Buffer& operator=(Buffer&& other)`
- `explicit RingBuffer(size_t capacity = kDefaultCapacity)`
- `size_t readable() const`
- `size_t writable() const`
- `size_t capacity() const`
- `bool empty() const`
- `bool full() const`
- `size_t getWriteIovecs(struct iovec* out, size_t max_iovecs = 2) const`
- `size_t getReadIovecs(struct iovec* out, size_t max_iovecs = 2) const`
- `void produce(size_t len)`
- `void consume(size_t len)`
- `void clear()`
- `size_t write(const void* data, size_t len)`
- `size_t write(const std::string_view& str)`

语义说明：

- `Buffer` 是 owning 动态缓冲区；`resize(...)` 最终调用 `reallocBytes(...)`，缩容时可能截断已有 `length()`
- `Buffer::clear()` 会把已有容量区间清零，但保留已分配容量，适合重复复用
- `RingBuffer` 由 `galay-utils/cache/ring_buffer.hpp` 提供，`galay-kernel` 只在 `common/buffer.h` 中保留 `galay::kernel::RingBuffer` using 入口
- `RingBuffer` 是固定容量、不会自动扩容的环形缓冲；写满后 `write(...)` / `produce(...)` 只会推进可容纳的那部分字节
- `getWriteIovecs(...)` / `getReadIovecs(...)` 最多返回两段连续内存，专门服务 `readv` / `writev`
- `consume(...)` 在把可读数据完全耗尽后，会把读写指针都重置到 `0`

## Runtime / Scheduler / Task / Timer

头文件：

- `galay-kernel/core/runtime.h`
- `galay-kernel/core/compute_scheduler.h`
- `galay-kernel/core/epoll_scheduler.h`
- `galay-kernel/core/kqueue_scheduler.h`
- `galay-kernel/core/uring_scheduler.h`
- `galay-kernel/core/task.h`
- `galay-kernel/core/scheduler.hpp`
- `galay-kernel/core/io_scheduler.hpp`
- `galay-kernel/core/timer_scheduler.h`
- `galay-kernel/common/timer.hpp`
- `galay-kernel/common/sleep.hpp`

关键类型：

- `struct RuntimeAffinityConfig`
- `struct RuntimeConfig`
- `class Runtime`
- `class RuntimeHandle`
- `class RuntimeBuilder`
- `template <typename T> class Task`
- `template <typename T> class JoinHandle`
- `class TaskRef`
- `class ComputeScheduler`
- `class EpollScheduler`
- `class KqueueScheduler`
- `class IOUringScheduler`
- `class Timer`
- `class CBTimer`
- `class TimerScheduler`

`RuntimeAffinityConfig` 字段：

- `enum class Mode { None, Sequential, Custom } mode`
- `size_t seq_io_count`
- `size_t seq_compute_count`
- `std::vector<uint32_t> custom_io_cpus`
- `std::vector<uint32_t> custom_compute_cpus`

`RuntimeConfig` 字段：

- `size_t io_scheduler_count = GALAY_RUNTIME_SCHEDULER_COUNT_AUTO`
- `size_t compute_scheduler_count = GALAY_RUNTIME_SCHEDULER_COUNT_AUTO`
- `RuntimeAffinityConfig affinity`

`Runtime` 关键接口：

- `explicit Runtime(const RuntimeConfig& config = RuntimeConfig{})`
- `template <typename T> T blockOn(Task<T> task)`
- `template <typename T> JoinHandle<T> spawn(Task<T> task)`
- `template <typename F> auto spawnBlocking(F&& func) -> JoinHandle<R>`
- `RuntimeHandle handle()`
- `bool addIOScheduler(std::unique_ptr<IOScheduler> scheduler)`
- `bool addComputeScheduler(std::unique_ptr<ComputeScheduler> scheduler)`
- `void start()`
- `void stop()`
- `bool isRunning() const`
- `size_t getIOSchedulerCount() const`
- `size_t getComputeSchedulerCount() const`
- `IOScheduler* getIOScheduler(size_t index)`
- `ComputeScheduler* getComputeScheduler(size_t index)`
- `IOScheduler* getNextIOScheduler()`
- `ComputeScheduler* getNextComputeScheduler()`

`RuntimeHandle` 关键接口：

- `static RuntimeHandle current()`
- `static std::optional<RuntimeHandle> tryCurrent()`
- `template <typename T> JoinHandle<T> spawn(Task<T> task) const`
- `template <typename F> auto spawnBlocking(F&& func) const -> JoinHandle<R>`

`RuntimeBuilder` 关键接口：

- `RuntimeBuilder& ioSchedulerCount(size_t n)`
- `RuntimeBuilder& computeSchedulerCount(size_t n)`
- `RuntimeBuilder& sequentialAffinity(size_t io_count, size_t compute_count)`
- `bool customAffinity(std::vector<uint32_t> io_cpus, std::vector<uint32_t> compute_cpus)`
- `RuntimeBuilder& applyAffinity(const RuntimeAffinityConfig& aff)`
- `Runtime build() const`
- `RuntimeConfig buildConfig() const`

注意：

- 高层任务入口统一收口到 `blockOn(...)`、`spawn(...)`、`spawnBlocking(...)`、`RuntimeHandle`
- `JoinHandle<T>` 的公开结果路径只有 `wait()` / `join()`；当前没有 `result()` 一类兼容接口
- `RuntimeHandle::current()` 在 runtime 上下文外会抛异常；更稳妥的探测入口是 `tryCurrent()`
- `Runtime::start()` 只有在 IO / Compute 两类调度器都还为空时才会自动创建默认调度器；如果你手工只添加了其中一类，另一类不会被自动补齐
- `Runtime::start()` 会先启动全局 `TimerScheduler`，再启动 IO / 计算调度器
- `Runtime::stop()` 会按相反顺序停止并回收
- 文档与示例应使用 `getIOScheduler(size_t)`、`getComputeScheduler(size_t)` 或 `getNext*()`；当前没有 `getIOSchedulers()` / `getComputeSchedulers()`
- `GALAY_RUNTIME_SCHEDULER_COUNT_AUTO` 表示自动数量；默认规则是 `io=2*CPU`、`compute=CPU`；`0` 表示禁用对应默认调度器
- 默认 IO 后端由平台和宏决定：macOS / FreeBSD 为 `KqueueScheduler`，Linux + `USE_IOURING` 为 `IOUringScheduler`，否则为 `EpollScheduler`
- `addIOScheduler(...)` / `addComputeScheduler(...)` 只允许在运行前调用；运行中会返回 `false`
- `getNextIOScheduler()` / `getNextComputeScheduler()` 采用 round-robin 轮询；对应容器为空时返回 `nullptr`
- `RuntimeBuilder::sequentialAffinity(io_count, compute_count)` 会从 CPU 0 开始顺序绑定，超出 CPU 数量后回绕
- `RuntimeBuilder::customAffinity(...)` 要求两个向量长度与当前配置的调度器数量严格一致，否则返回 `false` 且不修改配置；因此通常要先调用 `ioSchedulerCount(...)` / `computeSchedulerCount(...)`
- `Runtime::applyAffinityConfig()` 在 custom 向量和最终调度器数量不一致时会整体跳过 custom affinity 应用

`ComputeScheduler` 关键接口：

- `ComputeScheduler()`
- `void start()`
- `void stop()`
- `bool schedule(TaskRef task)`
- `bool scheduleDeferred(TaskRef task)`
- `bool scheduleImmediately(TaskRef task)`
- `bool isRunning() const`
- `bool addTimer(Timer::ptr timer)`

语义说明：

- `ComputeScheduler` 是单工作线程调度器，底层队列是 `moodycamel::BlockingConcurrentQueue<ComputeTask>`
- `schedule(...)` / `scheduleDeferred(...)` 会接收 `TaskRef`；若任务尚未绑定 owner scheduler，会绑定到当前 `ComputeScheduler`
- `scheduleImmediately(...)` 会在当前线程立即恢复任务；若任务已绑定到其他调度器，则返回 `false`
- `addTimer(...)` 只是转发到全局 `TimerScheduler::getInstance()->addTimer(...)`

具体 IO 调度器：

- `EpollScheduler(int max_events = GALAY_SCHEDULER_MAX_EVENTS, int batch_size = GALAY_SCHEDULER_BATCH_SIZE)`
- `KqueueScheduler(int max_events = GALAY_SCHEDULER_MAX_EVENTS, int batch_size = GALAY_SCHEDULER_BATCH_SIZE)`
- `IOUringScheduler(int queue_depth = GALAY_SCHEDULER_QUEUE_DEPTH, int batch_size = GALAY_SCHEDULER_BATCH_SIZE)`

这些后端当前共同公开：

- 生命周期：`start()`、`stop()`、`notify()`
- 事件注册：`addAccept(...)`、`addConnect(...)`、`addRecv(...)`、`addSend(...)`、`addReadv(...)`、`addWritev(...)`、`addClose(...)`
- 文件 / UDP / 监控 / 零拷贝：`addFileRead(...)`、`addFileWrite(...)`、`addRecvFrom(...)`、`addSendTo(...)`、`addFileWatch(...)`、`addSendFile(...)`、`addSequence(...)`
- 调度：`schedule(...)`、`scheduleDeferred(...)`、`scheduleImmediately(...)`
- 任务辅助：`scheduleTask(...)`、`scheduleTaskDeferred(...)`、`scheduleTaskImmediately(...)`
- 诊断：`int remove(IOController* controller)`、`std::optional<IOError> lastError() const`

平台边界：

- `EpollScheduler` 只在 `USE_EPOLL`
- `KqueueScheduler` 只在 `USE_KQUEUE`
- `IOUringScheduler` 只在 `USE_IOURING`
- 业务代码通常优先通过 `Runtime` 获取默认后端；只有在需要显式控制队列深度、批量参数或平台后端时才直接实例化这些类

## 低层组合 Awaitable 扩展面

头文件：

- `galay-kernel/core/awaitable.h`
- `galay-utils/cache/byte_queue_view.hpp`

公开类型：

- `template <typename ResultT, size_t InlineN = 4> class SequenceAwaitable`
- `template <typename ResultT, size_t InlineN = 4> class SequenceOps`
- `template <typename ResultT, size_t InlineN, typename FlowT, typename BaseContextT, auto Handler> struct SequenceStep`
- `template <typename ResultT, size_t InlineN, typename FlowT, auto Handler> struct LocalSequenceStep`
- `template <typename ResultT, size_t InlineN, typename FlowT, auto Handler> struct ParserSequenceStep`
- `template <typename ResultT, size_t InlineN = 4, typename FlowT = void> class AwaitableBuilder`
- `enum class MachineSignal { kContinue, kWaitRead, kWaitReadv, kWaitWrite, kWaitWritev, kWaitConnect, kComplete, kFail }`
- `template <typename ResultT> struct MachineAction`
- `template <typename MachineT> concept AwaitableStateMachine`
- `template <AwaitableStateMachine MachineT> class StateMachineAwaitable`
- `enum class ParseStatus { kNeedMore, kContinue, kCompleted }`
- `class ByteQueueView`

推荐用法：

- 线性组合步骤优先用 `AwaitableBuilder`
- 线性多段收发、header/body 分段 IO 优先 `AwaitableBuilder::readv(...) / writev(...)`
- 复杂双向协议、读写切换或 handshake/shutdown 状态推进优先用 `AwaitableBuilder::fromStateMachine(...)` 或直接 `StateMachineAwaitable<MachineT>`
- 需要显式持有步骤对象、跨步骤共享状态或自定义 re-arm 路径时使用 `SequenceAwaitable + SequenceStep`
- 协议解析优先使用 `AwaitableBuilder::parse(...)`，parse handler 返回 `ParseStatus`
- `ByteQueueView` 由 `galay-utils/cache/byte_queue_view.hpp` 提供，`galay-kernel` 不再保留本地 `queue_view.h`
- 链式 `AwaitableBuilder` 的 `build()` 现在返回 machine-backed awaitable，并与 `fromStateMachine(...)` 共享同一套状态机驱动

parse 语义：

- `ParseStatus::kNeedMore`：builder 会自动重挂最近一个非本地 IO 步骤（包括 `recv/readv`），然后再次进入 parse；协程保持挂起
- `ParseStatus::kContinue`：builder 会继续本地 parse loop，不等待新的内核事件
- `ParseStatus::kCompleted`：parse handler 已完成最终 `ops.complete(...)` 或显式排好了后续步骤

builder iovec 公开面：

- `template <auto Handler, size_t N> AwaitableBuilder& readv(std::array<struct iovec, N>& iovecs, size_t count = N)`
- `template <auto Handler, size_t N> AwaitableBuilder& readv(struct iovec (&iovecs)[N], size_t count = N)`
- `template <auto Handler, size_t N> AwaitableBuilder& writev(std::array<struct iovec, N>& iovecs, size_t count = N)`
- `template <auto Handler, size_t N> AwaitableBuilder& writev(struct iovec (&iovecs)[N], size_t count = N)`
- borrowed `iovecs` 以及底层 buffer 生命周期必须覆盖整个 awaitable 挂起期

状态机 / builder iovec 动作：

- `MachineAction<ResultT>::waitReadv(const struct iovec* iovecs, size_t count)`
- `MachineAction<ResultT>::waitWritev(const struct iovec* iovecs, size_t count)`
- `MachineSignal::kWaitReadv`：挂多段读事件，完成后仍通过 `onRead(std::expected<size_t, IOError>)` 回传总字节数
- `MachineSignal::kWaitWritev`：挂多段写事件，完成后仍通过 `onWrite(std::expected<size_t, IOError>)` 回传总字节数

状态机 connect 语义：

- `MachineSignal::kWaitConnect`：让内核注册 socket 连接完成
- 若状态机会返回 `kWaitConnect`，则需要实现 `onConnect(std::expected<void, IOError>)`
- 链式 builder 的 `.connect(...)` 也会通过同一套状态机驱动推进，而不是走独立旧路径
- machine-backed builder handler 不支持 `ops.queue(...)`；若需要显式排队步骤，请改用 `SequenceAwaitable + SequenceStep`

`ByteQueueView` 当前能力：

- `append(...)`：把新到达字节追加到队尾
- `size()` / `empty()` / `has(n)`：判断是否足够解析协议头或包体
- `view(offset, length)`：读取当前可见窗口内的连续字节视图
- `consume(length)`：消费已解析字节，并在合适时自动 compact

真实参考：

- 线性 builder：`test/t56_seqawait.cc`
- 显式步骤编排：`test/t25_await.cc`
- 半包不提前唤醒：`test/t59_needmore.cc`
- 粘包单次恢复尽量吃完：`test/t60_coalframe.cc`
- 状态机读写切换：`test/t69_smloop.cc`
- 状态机入口与 builder 桥接：`test/t70_smbridge.cc`
- 状态机错误动作：`test/t71_failact.cc`
- 零长度读写动作：`test/t72_zeroact.cc`
- builder connect 桥接：`test/t73_connbridge.cc`
- 自定义状态机 connect：`test/t74_connact.cc`
- builder queue 误用拒绝：`test/t75_qreject.cc`
- builder iovec surface：`test/t76_iov.cc`
- builder iovec 往返：`test/t77_iovrtp.cc`
- builder iovec parse 桥接：`test/t78_iovparse.cc`

任务辅助：

- `template <typename T> class Task`
- `template <typename T> class JoinHandle`
- `class TaskRef`
- `Task<void>& then(Task<void> next) &`
- `Task<void>&& then(Task<void> next) &&`
- `template <typename T> bool scheduleTask(Scheduler&, Task<T>&&)`
- `template <typename T> bool scheduleTaskDeferred(Scheduler&, Task<T>&&)`
- `template <typename T> bool scheduleTaskImmediately(Scheduler&, Task<T>&&)`
- `template<concepts::ChronoDuration Duration> SleepAwaitable sleep(Duration duration)`

`Task<T>` / `JoinHandle<T>` / `TaskRef`：

- `Task<T>`：公开任务返回类型；可直接 `co_await`，也可通过 `Runtime::blockOn(...)`、`Runtime::spawn(...)`、`RuntimeHandle::spawn(...)` 提交
- `JoinHandle<T>::wait()`：阻塞到结果就绪，但不提取结果
- `JoinHandle<T>::join()`：提取结果或重抛异常；结果只能消费一次
- `Task<void>::then(...)`：用于根任务链式串接，continuation 生命周期不依赖调用点临时对象
- `TaskRef`：轻量任务引用；主要供 scheduler/runtime 内核和低层测试使用

内部说明：

- `TaskRef`、`ComputeTask`、调度器绑定 / resume plumbing 已降级为 runtime/scheduler 内核实现细节，不再作为公开工作流 API 描述
- 需要排障或读实现时，以 `galay-kernel/core/task.h`、`scheduler.hpp` 与相关测试为准，不建议业务代码直接依赖这些内部类型

语义说明：

- `Task<T>` move 后仍共享同一底层任务状态；完成位和 continuation 挂接都在共享的 `TaskState` 上
- `co_await childTask()` 会先尝试把子任务立即提交到当前等待者所属调度器；若子任务尚未完成，则把等待者挂到子任务 continuation
- `Task<void>::then(...)` 仅承接根任务串接语义；当前没有泛型 `map/flatMap` 一类 combinator
- 被等待协程完成后，waiter 会通过其所属 `Scheduler::schedule(...)` 路径恢复，而不是直接跨线程恢复底层句柄
- 低层直接调度 task 时，推荐通过 `scheduleTask(...)` / `scheduleTaskDeferred(...)` / `scheduleTaskImmediately(...)` 包装辅助函数，而不是手工拆 `TaskRef`

`Timer` / `CBTimer` / `TimerScheduler`：

- `template<concepts::ChronoDuration Duration> Timer(Duration duration)`
- `virtual void handleTimeout()`
- `bool done()`
- `void cancel()`
- `bool cancelled()`
- `uint64_t getDelay() const`
- `uint64_t getExpireTime() const`
- `template<concepts::ChronoDuration Duration> CBTimer(Duration duration, std::function<void()>&& callback)`
- `static TimerScheduler* getInstance()`
- `void start()`
- `void stop()`
- `bool addTimer(Timer::ptr timer)`
- `size_t addTimerBatch(const std::vector<Timer::ptr>& timers)`
- `bool isRunning() const`
- `uint64_t tickDuration() const`
- `size_t size() const`

注意：

- `Timer` 把原始 duration 统一保存成纳秒延迟；绝对过期时间在第一次调用 `getExpireTime()` 时才懒计算并缓存
- `CBTimer::handleTimeout()` 只会在未取消且尚未完成时执行回调，随后再把自身标记为完成
- `sleep(...)` 最终调用 `TimerScheduler::getInstance()->addTimer(...)`
- 若全局 `TimerScheduler` 未启动，`sleep(...)` 所依赖的 `addTimer(...)` 会返回 `false`
- `tickDuration()` 是只读观测接口；当前没有公开 setter 来修改时间轮 tick
- `TimerScheduler` 是单例；`addTimerBatch(...)` 返回本次成功入队的定时器数量

## HandleOption / 网络 IO

头文件：

- `galay-kernel/common/handle_option.h`
- `galay-kernel/async/tcp_socket.h`
- `galay-kernel/async/udp_socket.h`

`HandleOption` 公开方法：

- `explicit HandleOption(GHandle handle)`
- `std::expected<void, IOError> handleBlock()`
- `std::expected<void, IOError> handleNonBlock()`
- `std::expected<void, IOError> handleReuseAddr()`
- `std::expected<void, IOError> handleReusePort()`

当前没有 `handleTcpNoDelay()`。

`TcpSocket` 关键接口：

- `explicit TcpSocket(IPType type = IPType::IPV4)`
- `explicit TcpSocket(GHandle handle)`
- `GHandle handle() const`
- `IOController* controller()`
- `HandleOption option()`
- `std::expected<void, IOError> bind(const Host& host)`
- `std::expected<void, IOError> listen(int backlog = 128)`
- `AcceptAwaitable accept(Host* clientHost)`
- `ConnectAwaitable connect(const Host& host)`
- `RecvAwaitable recv(char* buffer, size_t length)`
- `SendAwaitable send(const char* buffer, size_t length)`
- `template<size_t N> ReadvAwaitable readv(std::array<struct iovec, N>& iovecs, size_t count = N)`
- `template<size_t N> ReadvAwaitable readv(struct iovec (&iovecs)[N], size_t count = N)`
- `template<size_t N> WritevAwaitable writev(std::array<struct iovec, N>& iovecs, size_t count = N)`
- `template<size_t N> WritevAwaitable writev(struct iovec (&iovecs)[N], size_t count = N)`
- `SendFileAwaitable sendfile(int file_fd, off_t offset, size_t count)`
- `CloseAwaitable close()`

`UdpSocket` 关键接口：

- `explicit UdpSocket(IPType type = IPType::IPV4)`
- `explicit UdpSocket(GHandle handle)`
- `GHandle handle() const`
- `IOController* controller()`
- `HandleOption option()`
- `std::expected<void, IOError> bind(const Host& host)`
- `RecvFromAwaitable recvfrom(char* buffer, size_t length, Host* from)`
- `SendToAwaitable sendto(const char* buffer, size_t length, const Host& to)`
- `CloseAwaitable close()`

## 文件 IO 与文件监控

头文件：

- `galay-kernel/async/async_file.h`
- `galay-kernel/async/aio_file.h`
- `galay-kernel/async/file_watcher.h`
- `galay-kernel/core/watch_defs.hpp`

`AsyncFile`：

- 仅在 `USE_KQUEUE` 或 `USE_IOURING` 下公开
- `AsyncFile()`
- `std::expected<void, IOError> open(const std::string& path, FileOpenMode mode, int permissions = 0644)`
- `FileReadAwaitable read(char* buffer, size_t length, off_t offset = 0)`
- `FileWriteAwaitable write(const char* buffer, size_t length, off_t offset = 0)`
- `CloseAwaitable close()`
- `GHandle handle() const`
- `std::expected<size_t, IOError> size() const`
- `std::expected<void, IOError> sync()`

`AioFile`：

- 仅在 `USE_EPOLL` 下公开
- `AioFile(int max_events = 64)`
- `std::expected<void, IOError> open(const std::string& path, AioOpenMode mode, int permissions = 0644)`
- `void preRead(char* buffer, size_t length, off_t offset)`
- `void preWrite(const char* buffer, size_t length, off_t offset)`
- `void preReadBatch(const std::vector<std::tuple<char*, size_t, off_t>>& reads)`
- `void preWriteBatch(const std::vector<std::tuple<const char*, size_t, off_t>>& writes)`
- `AioCommitAwaitable commit()`
- `void clear()`
- `void close()`
- `GHandle handle() const`
- `bool isValid() const`
- `std::expected<size_t, IOError> size() const`
- `std::expected<void, IOError> sync()`
- `static char* allocAlignedBuffer(size_t size, size_t alignment = 512)`
- `static void freeAlignedBuffer(char* buffer)`

`FileWatcher`：

- `FileWatcher()`
- `std::expected<int, IOError> addWatch(const std::string& path, FileWatchEvent events = FileWatchEvent::All)`
- `std::expected<void, IOError> removeWatch(int wd)`
- `FileWatchAwaitable watch()`
- `bool isValid() const`
- `int fd() const`
- `std::string getPath(int wd) const`

`FileWatchEvent` / `FileWatchResult`：

- `enum class FileWatchEvent : uint32_t`
- `FileWatchEvent operator|(FileWatchEvent a, FileWatchEvent b)`
- `FileWatchEvent operator&(FileWatchEvent a, FileWatchEvent b)`
- `struct FileWatchResult { FileWatchEvent event; std::string name; bool isDir; bool has(FileWatchEvent check) const; }`

## 并发原语

头文件：

- `galay-kernel/concurrency/async_mutex.h`
- `galay-kernel/concurrency/mpsc_channel.h`
- `galay-kernel/concurrency/unsafe_channel.h`
- `galay-kernel/concurrency/async_waiter.h`

`AsyncMutex`：

- `explicit AsyncMutex(size_t initial_capacity = 32)`
- `AsyncMutexAwaitable lock()`
- `void unlock()`
- `bool isLocked() const`

`MpscChannel<T>`：

- `bool send(T&& value)`
- `bool send(const T& value)`
- `bool sendBatch(const std::vector<T>& values)`
- `bool sendBatch(std::vector<T>&& values)`
- `MpscRecvAwaitable<T> recv()`
- `MpscRecvBatchAwaitable<T> recvBatch(size_t maxCount = DEFAULT_BATCH_SIZE)`
- `std::optional<T> tryRecv()`
- `std::optional<std::vector<T>> tryRecvBatch(size_t maxCount = DEFAULT_BATCH_SIZE)`
- `size_t size() const`
- `bool empty() const`

注意：当前没有 `close()`，发送端也是同步 `bool send(...)`，不是 `co_await channel.send(...)`。

`UnsafeChannelWakeMode` / `UnsafeChannel<T>`：

- `enum class UnsafeChannelWakeMode { Inline, Deferred }`
- `explicit UnsafeChannel(UnsafeChannelWakeMode wake_mode = UnsafeChannelWakeMode::Inline)`
- `bool send(T&& value, bool immediately = false)`
- `bool send(const T& value, bool immediately = false)`
- `bool sendBatch(const std::vector<T>& values, bool immediately = false)`
- `bool sendBatch(std::vector<T>&& values, bool immediately = false)`
- `UnsafeRecvAwaitable<T> recv()`
- `UnsafeRecvBatchAwaitable<T> recvBatch(size_t maxCount = DEFAULT_BATCH_SIZE)`
- `UnsafeRecvBatchedAwaitable<T> recvBatched(size_t limit)`
- `std::optional<T> tryRecv()`
- `std::optional<std::vector<T>> tryRecvBatch(size_t maxCount = DEFAULT_BATCH_SIZE)`
- `size_t size() const`
- `bool empty() const`

语义说明：

- `UnsafeChannel<T>` 只允许同一调度器 / 同一线程上下文内使用；它不是线程安全容器
- `UnsafeChannelWakeMode::Inline` 会在生产者路径上直接 `handle.resume()` 唤醒 waiter
- `UnsafeChannelWakeMode::Deferred` 在 waiter 已绑定调度器时会走 `waiter.resume()` 的调度路径；如果 waiter 没有关联调度器，当前实现会回退为内联恢复
- `recvBatched(limit)` 只有在队列累计到 `limit` 条或发送端使用 `immediately=true` 时才会唤醒等待者
- `recv()` / `recvBatch()` / `recvBatched(...).timeout(...)` 超时后都会返回 `unexpected(IOError(kTimeout, 0))`
- `recvBatched(limit).timeout(...)` 当前不会自动把“未达到 limit 的部分数据”作为成功结果返回；部分数据会继续留在队列中，调用方可再用 `tryRecvBatch()` 或后续 `recv*()` 取出

`AsyncWaiter<T>`：

- `AsyncWaiterAwaitable<T> wait()`
- `bool notify(T result)`
- `bool isWaiting() const`
- `bool isReady() const`

`AsyncWaiter<void>`：

- `AsyncWaiterAwaitable<void> wait()`
- `bool notify()`
- `bool isWaiting() const`
- `bool isReady() const`

## 统一返回、所有权与并发语义

返回与错误语义：

- `bind/open/listen/sync` 这类立即执行接口通常直接返回 `std::expected<..., IOError>`
- `connect/accept/recv/send/read/write/sleep/lock/wait` 这类异步入口统一返回 awaitable，业务代码应直接 `co_await`
- 失败原因统一通过 `IOError` 或对应 awaitable 的 `await_resume()` 结果暴露，不应依赖日志文本推断
- 平台相关能力按头文件和编译宏裁剪：`AsyncFile` 只在 `USE_KQUEUE` / `USE_IOURING` 下可用，`AioFile` 只在 `USE_EPOLL` 下可用

所有权与生命周期：

- `Runtime::addIOScheduler(std::unique_ptr<IOScheduler>)` / `addComputeScheduler(std::unique_ptr<ComputeScheduler>)` 会转移调度器所有权给 `Runtime`
- `Bytes` 的构造函数族是 owning 深拷贝；`Bytes::fromString(...)` / `fromCString(...)` 是 non-owning 视图，底层存储必须由调用方保活
- `Buffer` 持有自己的动态缓冲；`RingBuffer` 也是 owning 固定容量缓冲，但不会自动扩容
- `sleep(...)` 依赖全局 `TimerScheduler`；如果运行时还没启动计时器，`sleep` 对应的底层定时器注册不会成功
- `TcpSocket` / `UdpSocket` / `AsyncFile` / `AioFile` / `FileWatcher` 都围绕底层句柄工作，关闭后不应继续复用旧句柄语义

并发边界：

- `MpscChannel<T>` 的发送端是同步 `bool send(...)`，不是 awaitable；不要写成 `co_await channel.send(...)`
- `UnsafeChannel<T>` 是显式的非线程安全通道，只适合同线程 / 受控调度上下文
- `UnsafeChannelWakeMode::Deferred` 只是把唤醒动作切到调度器排队路径，不会把 `UnsafeChannel<T>` 变成跨线程安全 channel
- `AsyncWaiter<T>` / `AsyncMutex` 用于协程同步，不是跨进程或跨机器同步原语

## 交叉验证入口

- 当前工作树未包含受版本控制的 package consumer fixture
- 公开 API 面当前主要通过 `test/` 与 `examples/` 交叉验证
- 调度 / 运行时：`test/t10_compute.cc`、`test/t11_mixed.cc`
- task / sleep：`test/t1_chain.cc`
- 并发：`test/t12_mutex.cc`、`test/t13_mpsc.cc`、`test/t15_unsafe.cc`
- 定时器：`test/t14_wheel.cc`、`test/t16_timer.cc`
- 真实示例总览：`docs/04-示例代码.md`

## 专题问题优先落点

- `galay.kernel` / `Runtime` / `RuntimeBuilder` / `IOScheduler` / `ComputeScheduler` / `Task` / `TimerScheduler`：
  - 先看本页 `模块门面 galay.kernel`、`Runtime / Scheduler / Task / Timer`
  - 再看 `docs/01-架构设计.md` 与 `docs/03-使用指南.md`
- `Bytes` / `ByteMetaData` / `Buffer` / `RingBuffer` / `IOError` / `Host` / `IPType`：
  - 先看本页 `地址、错误与字节缓冲工具`
  - 再看 `docs/03-使用指南.md`、`docs/07-常见问题.md`
- `TcpSocket` / `UdpSocket` / `HandleOption`：
  - 先看本页 `HandleOption / 网络 IO`
  - 再看 `docs/03-使用指南.md` 与 `docs/06-高级主题.md`
- `AsyncFile` / `AioFile` / `FileWatcher`：
  - 先看本页 `文件 IO 与文件监控`
  - 再看 `docs/03-使用指南.md`、`docs/06-高级主题.md`
- `AsyncMutex` / `MpscChannel<T>` / `UnsafeChannel<T>` / `AsyncWaiter<T>`：
  - 先看本页 `并发原语`
  - 再看 `docs/03-使用指南.md` 与 `docs/07-常见问题.md`
- `find_package(galay)` / 已安装头文件边界：
  - 先看本页 `导出 target`、`安装包头文件边界`
  - 再看 `docs/00-快速开始.md` 与 `docs/07-常见问题.md`

## 进一步阅读

- 工作流示例：`docs/03-使用指南.md`
- 真实示例矩阵：`docs/04-示例代码.md`
- 当前性能事实：`docs/05-性能测试.md`
- 平台差异、绑核、零拷贝、文件监控：`docs/06-高级主题.md`
- 补充专题落地页：`docs/08-计算调度器.md` 到 `docs/20-异步同步原语.md`
