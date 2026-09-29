# 02-API参考

本页按公开头文件整理当前可见 API。若文档与源码冲突，以 `galay-utils/*.hpp` 为准。

## 1. 公开入口

| 入口 | 真实文件 / target | 说明 |
|---|---|---|
| 细粒度头文件 | `galay-utils/<module>/*.hpp` | 推荐作为最小依赖接入面 |
| umbrella header | `galay-utils/galay_utils.hpp` | 聚合常用公开头，不默认导出 `RateLimiter` |
| C++23 模块 | `galay-utils/module/galay_utils.cppm` | 通过 `import galay.utils;` 导入，导出面与 umbrella 基本一致 |

完整公开头文件清单：

- `galay-utils/galay_utils.hpp`
- `galay-utils/core/string.hpp`
- `galay-utils/core/random.hpp`
- `galay-utils/system/system.hpp`
- `galay-utils/system/env.hpp`
- `galay-utils/core/time.hpp`
- `galay-utils/core/type_name.hpp`
- `galay-utils/system/backtrace.hpp`
- `galay-utils/system/signal.hpp`
- `galay-utils/concurrency/thread.hpp`
- `galay-utils/concurrency/pool.hpp`
- `galay-utils/cache/lru_cache.hpp`
- `galay-utils/buffer/bytes.hpp`
- `galay-utils/buffer/byte_queue_view.hpp`
- `galay-utils/buffer/ring_buffer.hpp`
- `galay-utils/resilience/rate_limiter.hpp`
- `galay-utils/resilience/circuit_breaker.hpp`
- `galay-utils/algorithm/balancer.hpp`
- `galay-utils/algorithm/consistent_hash.hpp`
- `galay-utils/algorithm/bloom_filter.hpp`
- `galay-utils/algorithm/trie.hpp`
- `galay-utils/algorithm/mvcc.hpp`
- `galay-utils/encoding/huffman.hpp`
- `galay-utils/app/app.hpp`
- `galay-utils/config/parser_manager.hpp`
- `galay-utils/system/process.hpp`
- `galay-utils/system/performance.hpp`
- `galay-utils/encoding/base64.hpp`
- `galay-utils/crypto/md5.hpp`
- `galay-utils/algorithm/murmur_hash3.hpp`
- `galay-utils/crypto/salt.hpp`
- `galay-utils/crypto/hmac.hpp`
- `galay-utils/common/defn.hpp`
- `galay-utils/module/module_prelude.hpp`
- `galay-utils/module/galay_utils.cppm`

## 2. 核心工具

| 模块 | 头文件 | 主要类型 / 函数 |
|---|---|---|
| String | `galay-utils/core/string.hpp` | `StringUtils` |
| Random | `galay-utils/core/random.hpp` | `RandomGenerator`、`Randomizer` |
| Time | `galay-utils/core/time.hpp` | `Time`、`StopWatch<Clock>`、`Deadline<Clock>`、`Backoff` |
| System | `galay-utils/system/system.hpp` | `System`、`System::AddressType` |
| Env | `galay-utils/system/env.hpp` | `Env` |
| TypeName | `galay-utils/core/type_name.hpp` | `getTypeName<T>()`、`getTypeName(obj)`、`demangleSymbol()` |
| BackTrace | `galay-utils/system/backtrace.hpp` | `BackTrace` |
| Signal | `galay-utils/system/signal.hpp` | `SignalHandler` |
| Performance | `galay-utils/system/performance.hpp` | `Performance`、`NumaPolicy`、`NumaPolicyState` |

### `Performance`：CPU / NUMA 放置

公开头为 `<galay/cpp/galay-utils/system/performance.hpp>`，也从 umbrella 和 `galay.utils` 模块导出，与 `Env`、`System` 一起归入 `system/` 目录。

| 静态方法 | 返回值 / 契约 |
|---|---|
| `cpuAffinity()` | 当前调用线程的有效逻辑 CPU ID 集合，按 ID 排序 |
| `bindCurrentThread(cpus)` | 设置当前线程，返回系统回读的实际 CPU 集合；重复 ID 合并 |
| `allowedNumaNodes()` | 当前 cpuset 允许使用的内存节点 |
| `numaPolicy()` | 当前线程的原生策略值（包含标志位）和节点集合 |
| `setNumaPolicy(policy, nodes)` | 设置 `Default` / `Bind` / `Interleave`；成功返回空 expected |

所有方法使用 `std::expected<T, std::error_code>`。系统调用失败立即保留原始 errno，参数错误为 `invalid_argument`，非 Linux 平台为 `operation_not_supported`；不抛出或捕获异常处理可恢复错误。调用方必须检查结果。

- 作用于**当前线程**，不隐式修改已有工作线程。Linux 新建线程继承创建者当时的 CPU mask 和 NUMA 策略，应在创建线程前配置。
- `bindCurrentThread` 的回读结果可能是内核与 online / cpuset 限制的交集；调用方要比较实际集合与请求。需要保留 taskset 边界时，先验证请求是启动 mask 的子集。回读失败时绑定可能已经生效，此接口不承诺事务式回滚。
- `Default` 要求空节点集；`Bind` / `Interleave` 要求非空集。设置后可用 `numaPolicy()` 回读验证；压测公共入口会自动检查允许节点和精确回读。
- CPU ID 范围由 `CPU_SETSIZE` 决定，当前 Linux 实现为 0–1023；NUMA 节点范围为 0–1023。超范围请求明确失败；读取系统 mask 的容量不足会返回内核错误，不截断集合。
- NUMA 策略影响后续内存分配和首次触碰，不迁移已有页；静态初始化、已触碰的分配器缓存、显式 VMA 策略等不因此改变。CPU 绑定与内存节点绑定是独立设置，需按机器拓扑选择。
- 只包装内核现有 affinity / mempolicy 接口，不增加 libnuma 依赖，不修改频率、实时调度、IRQ 或系统全局配置。

压测配置、示例及限制见 [05-性能测试：CPU / NUMA 启动控制](05-性能测试.md#2026-09-29cpu--numa-启动控制)。

### `StringUtils`

- `split(std::string_view, char)`
- `split(std::string_view, std::string_view)`
- `splitRespectQuotes(std::string_view, char, char)`
- `join(const std::vector<std::string>&, std::string_view)`
- `trim` / `trimLeft` / `trimRight`
- `toLower` / `toUpper`
- `startsWith` / `endsWith` / `contains`
- `replace` / `replaceFirst`
- `count(char)` / `count(std::string_view)`
- `toHex` / `fromHex` / `toVisibleHex`
- `isInteger` / `isFloat` / `isBlank`
- `format(...)`
- `parse<T>(...)`
- `toString(...)`
- 语义：
  - 纯静态工具，不持有共享状态，线程安全性由输入输出对象自身决定
  - `split(..., "")` 返回原字符串；连续分隔符会保留空字段
  - `splitRespectQuotes(...)` 只按 quote 状态忽略分隔符，不负责校验 quote 是否成对
  - `toHex(nullptr, *)`、`toVisibleHex(nullptr, *)`、奇数长度或包含非法字符的 `fromHex(...)` 返回空结果
  - `parse<T>(...)` 要求去除首尾空白后完整解析；溢出、空串或尾随非法字符返回默认值

### `RandomGenerator` / `Randomizer`

- `RandomGenerator()`
- `explicit RandomGenerator(uint64_t seedValue)`
- `RandomGenerator::seed()` / `RandomGenerator::reseed()`
- `static Randomizer& instance()`
- `randomInt` / `randomUint32` / `randomUint64`
- `randomDouble` / `randomFloat` / `randomBool`
- `randomString` / `randomHex` / `randomBytes`
- `uuid()`
- `seed()` / `reseed()`
- 语义：
  - `RandomGenerator` 是本地无锁生成器，非线程安全；共享同一个实例时必须由调用方外部加锁
  - `Randomizer` 是线程安全单例，内部用 mutex 保护共享随机引擎；可跨线程共享，但不适合协程热路径高频调用
  - 整数随机返回闭区间 `[min, max]`；浮点随机返回半开区间 `[min, max)`；`min >= max` 时返回 `min`
  - `randomBool(probability)` 对概率做边界处理：`<= 0` 返回 `false`，`>= 1` 返回 `true`
  - `randomString(0, *)`、`randomString(*, "")`、`randomHex(0)` 返回空字符串；`randomBytes(nullptr, *)` 为 no-op
  - `uuid()` 生成 RFC 4122 version 4 形态字符串，variant 位落在 `8`/`9`/`a`/`b`

### `Time`

- `Time::currentTimeMs()` / `Time::currentTimeUs()` / `Time::currentTimeNs()`
- `Time::formatTime(std::time_t timestamp, const char* format, bool utc = false)`
- `Time::currentGMTTime(const char* format = "%a, %d %b %Y %H:%M:%S GMT")`
- `Time::currentLocalTime(const char* format = "%Y-%m-%d %H:%M:%S")`
- `StopWatch<Clock>`
  - `StopWatch()` / `explicit StopWatch(time_point start)`
  - `reset()`
  - `elapsed()` / `elapsedMs()`
  - `startTime()`
- `Deadline<Clock>`
  - `explicit Deadline(time_point deadline)`
  - `fromNow(duration)`
  - `expired()` / `remaining()`
  - `timePoint()`
- `Backoff`
  - `Backoff::fixed(duration)` / `Backoff::linear(initial, step, max)` / `Backoff::exponential(initial, multiplier, max)`
  - `next()` / `reset()`
  - `attempts()` / `strategy()`
- 语义：
  - `System` 不再提供 `currentTime*`、`currentGMTTime`、`currentLocalTime` 或 `formatTime`；时间相关能力统一使用 `Time`
  - `formatTime(...)` 的 `format == nullptr`、空格式、平台时间转换失败或格式化结果写入失败时返回空字符串
  - `StopWatch`、`Deadline`、`Backoff` 都是轻量非线程安全值对象，不创建线程，不提供 sleep 或调度语义
  - 这些类型不依赖平台、进程或 signal 头文件

### `System`

- 文件：`System::readFile` / `System::writeFile` / `System::readFileMmap`
- 文件系统：`System::fileExists` / `System::isDirectory` / `System::fileSize` / `System::createDirectory` / `System::remove` / `System::listDirectory`
- 网络：`System::resolveHostIPv4` / `System::resolveHostIPv6` / `System::checkAddressType`
- 主机：`System::cpuCount` / `System::hostname` / `System::currentDir` / `System::changeDir` / `System::executablePath`
- 语义：系统时间戳和时间格式化 API 使用 `Time`；进程环境变量使用 `Env`

### `Env`

- 细粒度入口：`#include <galay/cpp/galay-utils/system/env.hpp>`；总头和 `import galay.utils;` 同样导出
- `Env::get(const std::string& name) -> std::expected<std::optional<std::string>, std::error_code>`
- `Env::set(const std::string& name, const std::string& value, bool overwrite = true) -> std::expected<void, std::error_code>`
- `Env::unset(const std::string& name) -> std::expected<void, std::error_code>`
- 语义：
  - `get` 返回独立字符串副本；不存在时成功返回 `std::nullopt`，POSIX 上存在但为空的变量返回空字符串
  - `set(..., false)` 保留已有变量（包括 POSIX 空值）；不存在时创建。Windows CRT 将空值视为删除
  - `unset` 对不存在的变量也成功
  - 名称不能为空或包含 `=`、NUL；值不能包含 NUL。非法输入返回 `std::errc::invalid_argument`，不会修改环境
  - 系统失败通过 `std::error_code` 保留 errno / CRT 原始错误码；必须先检查 `expected`，再读取结果或错误
  - 直接操作进程共享环境，不缓存、不加锁；调用方须避免与其他环境读写并发，建议在启动工作线程/协程前完成修改
  - `.env` 文件解析仍由 `EnvParser` 提供，不会自动写入进程环境

### `TypeName`

- `template<typename T> getTypeName() -> std::string`
- `template<typename T> getTypeName(const T& obj) -> std::string`
- `demangleSymbol(const char* mangledName) -> std::string`
- 语义：GCC / Clang 下会尝试 demangle；失败或平台不支持时返回原始 `typeid(...).name()` / 符号名；`nullptr` 输入返回空字符串
  - 纯函数式工具，不持有共享可变状态

### `BackTrace`

- `getStackTrace(int maxFrames = 64, int skipFrames = 1) -> std::vector<std::string>`
- `printStackTrace(int maxFrames = 64, int skipFrames = 1)`
- `getStackTraceString(int maxFrames = 64, int skipFrames = 1) -> std::string`
- `installCrashHandlers()`
- 语义：
  - 当前只在 `__APPLE__` / `__linux__` 上真正采集堆栈；其他平台会返回空栈或只输出 `0 frames`
  - `printStackTrace(...)` / `getStackTraceString(...)` 会在传入的 `skipFrames` 基础上再额外跳过 1 帧，用来隐藏包装函数自身
  - `installCrashHandlers()` 会安装 `SIGSEGV` / `SIGABRT` / `SIGFPE` / `SIGILL`，在支持的平台上额外安装 `SIGBUS`
  - crash handler 打印堆栈后会把该 signal 的处理方式恢复为 `SIG_DFL`，再重新 `raise(signal)`，因此进程仍会按默认方式终止

### `SignalHandler`

- `using Handler = std::function<void(int)>`
- `static SignalHandler& instance()`
- `bool setHandler(int signal, Handler handler)`
- `template<int... Signals> bool setHandler(Handler handler)`
- `bool removeHandler(int signal)` / `bool restoreDefault(int signal)`
- `bool ignoreSignal(int signal)`
- `bool blockSignal(int signal)` / `bool unblockSignal(int signal)`
- `bool hasHandler(int signal) const`
- 语义：
  - `setHandler(...)` / `removeHandler(...)` / `restoreDefault(...)` / `ignoreSignal(...)` / `blockSignal(...)` / `unblockSignal(...)` 都返回 `bool`
  - Windows 下 `setHandler(...)` / `removeHandler(...)` / `ignoreSignal(...)` 走 `std::signal(...)`；`blockSignal()` / `unblockSignal()` 固定返回 `false`
  - POSIX 下 `setHandler(...)` 使用 `sigaction(..., SA_RESTART, ...)` 注册进程级 signal handler
  - `blockSignal(...)` / `unblockSignal(...)` 在 POSIX 下通过 `pthread_sigmask(...)` 修改的是当前线程的 signal mask，而不是全局进程 mask

## 3. 并发、缓存与缓冲

| 模块 | 头文件 | 主要类型 |
|---|---|---|
| Cache | `galay-utils/cache/lru_cache.hpp` | `LruCache<Key, Value, Hash, KeyEqual, Clock, EnableStats>` |
| Bytes | `galay-utils/buffer/bytes.hpp` | `Bytes`、`ByteMetaData` |
| ByteQueueView | `galay-utils/buffer/byte_queue_view.hpp` | `ByteQueueView` |
| RingBuffer | `galay-utils/buffer/ring_buffer.hpp` | `RingBuffer` |
| Thread | `galay-utils/concurrency/thread.hpp` | `ThreadPool`、`TaskWaiter` |
| Pool | `galay-utils/concurrency/pool.hpp` | `PoolableObject`、`ObjectPool<T>`、`BlockingObjectPool<T>` |

### `LruCache`

- 模板参数：`Key`、`Value`、`Hash = std::hash<Key>`、`KeyEqual = std::equal_to<Key>`、`Clock = std::chrono::steady_clock`、`EnableStats = false`
- 类型：
  - `EvictReason`：`Capacity` / `Expired` / `Removed` / `Cleared`
  - `ExpirationPolicy`：`ExpireAfterWrite` / `ExpireAfterAccess`
  - `Stats`：`hits` / `misses` / `inserts` / `updates` / `capacityEvictions` / `expiredEvictions` / `removes` / `clears`
  - `EvictCallback = std::function<void(const Key&, const Value&, EvictReason)>`
- 构造：
  - `LruCache(size_type capacity = 0, std::optional<duration> defaultTtl = std::nullopt, EvictCallback onEvict = nullptr, ExpirationPolicy expirationPolicy = ExpirationPolicy::ExpireAfterWrite)`
  - `LruCache(size_type capacity, chrono duration defaultTtl, EvictCallback onEvict = nullptr, ExpirationPolicy expirationPolicy = ExpirationPolicy::ExpireAfterWrite)`
- 写入：`put` / `putFor` / `putUntil` / `emplace` / `emplaceFor`
- 查询：`get` / `peek` / `contains`
- 管理：`remove` / `clear` / `size` / `empty` / `capacity` / `setCapacity` / `defaultTtl` / `setDefaultTtl` / `purgeExpired`
- 哈希表调优：`reserve(size_type)` / `maxLoadFactor(float)` / `maxLoadFactor()`
- 统计：`statsEnabled()` / `stats()` / `resetStats()`
- 语义：
  - 非线程安全；多线程或跨协程并发访问同一个实例时必须由调用方外部同步
  - 容量淘汰和 TTL 淘汰都是惰性的，不创建后台线程或定时器
  - 统计默认关闭；只有 `EnableStats = true` 的实例才在热路径累计计数
  - 纯容量 LRU 未配置 TTL 条目时不访问 `Clock::now()`

### `Bytes` / `ByteMetaData`

`ByteMetaData`：

- `struct ByteMetaData { uint8_t* data; size_t size; size_t capacity; }`
- `ByteMetaData()` / `ByteMetaData(std::string&)` / `ByteMetaData(std::string_view)`
- `ByteMetaData(const char*)` / `ByteMetaData(const uint8_t*)`
- `ByteMetaData(const char*, size_t)` / `ByteMetaData(const uint8_t*, size_t)`
- `mallocBytes(size_t)` / `deepCopyBytes(const ByteMetaData&)`
- `reallocBytes(ByteMetaData&, size_t)` / `clearBytes(ByteMetaData&)` / `freeBytes(ByteMetaData&)`

`Bytes`：

- move-only：支持移动构造和移动赋值，不支持拷贝
- 构造：`Bytes()` / `Bytes(std::string&)` / `Bytes(std::string&&)` / `Bytes(const char*)` / `Bytes(const uint8_t*)`
- 构造：`Bytes(const char*, size_t)` / `Bytes(const uint8_t*, size_t)` / `explicit Bytes(size_t capacity)`
- 非拥有视图：`Bytes::fromString(std::string&)` / `Bytes::fromString(std::string_view)` / `Bytes::fromCString(const char*, size_t, size_t)`
- 查询：`data()` / `c_str()` / `size()` / `capacity()` / `empty()`
- 转换：`toString()` / `toStringView()`
- 管理：`clear()`
- 比较：`operator==` / `operator!=`
- 语义：
  - owning 构造函数会深拷贝输入字节；`Bytes` 析构或 `clear()` 时释放拥有的内存
  - `fromString(...)` / `fromCString(...)` 只创建 non-owning 视图，调用方必须保证底层存储生命周期长于 `Bytes`
  - `ByteMetaData` 是底层原始指针/大小/容量描述结构，本身不表达所有权
  - `c_str()` 只有在 `capacity() > size()` 时才会补写 null 终止符，不会越界扩容
  - 非线程安全；并发访问同一个实例时必须由调用方外部同步

### `ByteQueueView`

- `ByteQueueView()` / `explicit ByteQueueView(size_t reserveSize)`
- `reserve(size_t capacity)`
- `append(const char* data, size_t length)` / `append(std::string_view)` / `append(std::span<const std::byte>)`
- `size()` / `empty()` / `has(size_t length)`
- `data()`
- `view(size_t offset, size_t length)`
- `consume(size_t length)`
- `clear()`
- 语义：仅追加、头部消费的连续字节队列视图；已消费区域达到阈值后惰性压缩；非线程安全

### `RingBuffer`

- `explicit RingBuffer(size_t capacity = RingBuffer::kDefaultCapacity)`
- move-only：支持移动构造和移动赋值，不支持拷贝
- 状态：`readable()` / `writable()` / `capacity()` / `empty()` / `full()`
- 视图：`writeSpans(std::array<std::span<std::byte>, 2>&)` / `readSpans(std::array<std::span<const std::byte>, 2>&)`
- POSIX I/O 视图：`getWriteIovecs(...)` / `getReadIovecs(...)`
- 指针推进：`produce(size_t)` / `consume(size_t)`
- 数据复制：`write(const void*, size_t)` / `write(std::string_view)` / `read(void*, size_t)`
- `clear()`
- 语义：
  - `capacity == 0` 构造会抛 `std::invalid_argument`
  - `produce()` / `consume()` 超过可写或可读数量时自动截断
  - POSIX `iovec` 方法只在支持 `<sys/uio.h>` 的平台可见
  - 非线程安全；并发访问时必须由调用方外部同步

### `ThreadPool`

- `ThreadPool(size_t numThreads = 0)`
- `addTask(F&&, Args&&...) -> std::future<std::invoke_result_t<F, Args...>>`
- `execute(F&&)`
- `threadCount()` / `pendingTasks()` / `isStopped()`
- `waitAll()`
- `stop()` / `stopNow()`
- 语义：`addTask(...)` 在池已停止时抛 `std::runtime_error`；`execute(...)` 只派发任务，不返回 `future`
- 实现：任务队列基于 `moodycamel::BlockingConcurrentQueue<std::function<void()>>`；提交路径不使用 `std::mutex` / `std::condition_variable`
- 阻塞：`waitAll()`、`stop()`、`stopNow()` 仍会阻塞调用线程，不是 coroutine awaitable

### `TaskWaiter`

- `addTask(ThreadPool&, F&&)`
- `wait()`
- `waitFor(timeout)`
- 实现：使用原子计数和 `atomic::wait/notify_all` 等待，不再使用 mutex/condition_variable

### `PoolableObject` / `IsPoolable<T>`

- `virtual ~PoolableObject() = default`
- `virtual void reset()`
- `IsPoolable<T>`：`T` 继承 `PoolableObject`，或自行提供 `reset() -> void`

### `ObjectPool<T>`

- `using Ptr = std::unique_ptr<T, std::function<void(T*)>>`
- `using Creator = std::function<T*()>`
- `using Destroyer = std::function<void(T*)>`
- `ObjectPool(size_t initialSize = 0, size_t maxSize = 0, Creator creator = nullptr, Destroyer destroyer = nullptr)`
- `acquire()`：优先复用池内对象；池空时按需新建
- `tryAcquire()`：仅在池内已有对象时成功，否则返回空 `Ptr`
- `size()` / `totalCreated()` / `empty()`
- `clear()` / `shrink(size_t targetSize)`

### `BlockingObjectPool<T>`

- `using Ptr = std::unique_ptr<T, std::function<void(T*)>>`
- `using Creator = std::function<T*()>`
- `using Destroyer = std::function<void(T*)>`
- `BlockingObjectPool(size_t poolSize, Creator creator = nullptr, Destroyer destroyer = nullptr)`
- `acquire()`：阻塞直到池内有对象可取
- `tryAcquireFor(timeout)`：超时返回空 `Ptr`
- `available()`
- 语义：这是固定大小阻塞池；没有 `tryAcquire()`、`totalCreated()`、`clear()`、`shrink()` 这组 API

## 4. 流控与容错

| 模块 | 头文件 | 主要类型 |
|---|---|---|
| RateLimiter | `galay-utils/resilience/rate_limiter.hpp` | `CountingSemaphore`、`TokenBucketLimiter`、`SlidingWindowLimiter`、`LeakyBucketLimiter` |
| CircuitBreaker | `galay-utils/resilience/circuit_breaker.hpp` | `CircuitState`、`CircuitBreakerError`、`CircuitBreakerExpected`、`CircuitBreakerConfig`、`BasicCircuitBreaker`、`CircuitBreaker` |

### `RateLimiter`

`rate_limiter.hpp` 的公开面分为四个无锁同步非阻塞限流器类型；异步 `acquire()` / awaitable 路径已移除，避免引入 `galay-kernel`。`tryAcquire(...)` 成功返回 `true`，未通过限流直接返回 `false`：

- `CountingSemaphore`
  - `tryAcquire(size_t n = 1)`
  - `release(size_t n = 1)`
  - `available()`
- `TokenBucketLimiter`
  - `TokenBucketLimiter(double rate, size_t capacity)`
  - `tryAcquire(size_t tokens = 1)`
  - `availableTokens()`
  - `setRate(double)` / `setCapacity(size_t)`
  - `rate()` / `capacity()`
- `SlidingWindowLimiter`
  - `SlidingWindowLimiter(size_t maxRequests, std::chrono::milliseconds windowSize)`
  - `tryAcquire()`
  - `maxRequests()`
  - `windowSize()`
- `LeakyBucketLimiter`
  - `LeakyBucketLimiter(double rate, size_t capacity)`
  - `tryAcquire(size_t amount = 1)`
  - `currentWater()`
  - `rate()` / `capacity()`

依赖边界：

- 该头文件仅依赖标准库
- 不再提供异步限流器，不再依赖 `galay-kernel`；`galay/thirdparty/concurrentqueue/moodycamel` 仅用于线程池任务队列
- 限流器内部使用原子状态与 CAS，不使用内部互斥锁；需要 coroutine awaitable 时由上层运行时适配

### `CircuitBreaker`

- `CircuitBreakerError`
  - `Open`：熔断器打开，`execute(F&&)` 未执行主函数
- `CircuitBreakerExpected<T>`：约束 `execute` / `executeWithFallback` 接受的 expected-like 返回类型；函数需按值返回该类型，类型需要提供 `value_type`、`error_type` 和 `has_value()`
- `CircuitBreakerConfig`
  - `failureThreshold`
  - `successThreshold`
  - `halfOpenMaxRequests`
  - `resetTimeout`
- `BasicCircuitBreaker<ClockType = std::chrono::steady_clock>`：可注入时钟源的熔断器模板，适合确定性测试或自定义时间源
- `CircuitBreaker`
  - `using CircuitBreaker = BasicCircuitBreaker<>`
  - `allowRequest()`
  - `onSuccess()` / `onFailure()`
  - `execute(F&&)`：主函数必须返回 expected-like 结果，且 `error_type` 可从 `CircuitBreakerError` 构造；成功结果记录成功，失败结果记录失败；熔断打开时返回包含 `CircuitBreakerError::Open` 的失败结果
  - `executeWithFallback(F&&, Fallback&&)`：主函数和 fallback 必须返回同一 expected-like 类型；主函数失败或熔断打开时返回 fallback 结果
  - `state()` / `stateString()`
  - `failureCount()` / `successCount()`
  - `reset()` / `forceOpen()`
  - `config()`
  - 语义：执行接口不捕获异常，也不通过异常判断失败；调用方应通过 `std::expected` 或兼容的结果类型表达业务失败
  - 配置归一化：`failureThreshold`、`successThreshold`、`halfOpenMaxRequests` 小于 1 时按 1 处理；负的 `resetTimeout` 按 0 处理
  - 半开语义：`halfOpenMaxRequests` 限制半开状态下同时放行的探测请求数，探测成功或失败后释放名额

## 5. 算法与数据结构

| 模块 | 头文件 | 主要类型 / 方法 |
|---|---|---|
| Balancer | `galay-utils/algorithm/balancer.hpp` | `RoundRobinLoadBalancer<T>`、`WeightRoundRobinLoadBalancer<T>`、`RandomLoadBalancer<T>`、`WeightedRandomLoadBalancer<T>` |
| ConsistentHash | `galay-utils/algorithm/consistent_hash.hpp` | `NodeConfig`、`NodeStatus`、`PhysicalNode`、`ConsistentHash` |
| BloomFilter | `galay-utils/algorithm/bloom_filter.hpp` | `BloomFilter<T, Hash>` |
| Trie | `galay-utils/algorithm/trie.hpp` | `TrieTree` |
| MVCC | `galay-utils/algorithm/mvcc.hpp` | `VersionedValue<T>`、`Mvcc<T>`、`Snapshot`、`Transaction<T>` |
| MurmurHash3 | `galay-utils/algorithm/murmur_hash3.hpp` | `MurmurHash3Util` |

### `MurmurHash3Util`

- `Hash32(const void*, size_t, uint32_t seed = 0)` / `Hash32(const std::string&, uint32_t seed = 0)`
- `Hash128(const void*, size_t, uint32_t seed = 0)` / `Hash128(const std::string&, uint32_t seed = 0)`
- `Hash128Raw(const void*, size_t, uint32_t seed = 0)` / `Hash128Raw(const std::string&, uint32_t seed = 0)`
- C++17：`Hash32View(std::string_view, uint32_t seed = 0)`、`Hash128View(std::string_view, uint32_t seed = 0)`、`Hash128RawView(std::string_view, uint32_t seed = 0)`
- 语义：`Hash32(...)` 返回 32 位整数；`Hash128(...)` / `Hash128View(...)` 返回 32 字符十六进制字符串；`Hash128Raw(...)` / `Hash128RawView(...)` 返回 `std::array<uint64_t, 2>`

### `Balancer`

- `RoundRobinLoadBalancer<T>`：`select()` / `size()` / `append(Type)`
- `WeightRoundRobinLoadBalancer<T>`：`select()` / `size()` / `append(Type, uint32_t)`
- `RandomLoadBalancer<T>`：`select()` / `size()` / `append(Type)`
- `WeightedRandomLoadBalancer<T>`：`select()` / `size()` / `append(Type, uint32_t)`

### `ConsistentHash`

- `NodeStatus`
  - 数据成员：`healthy` / `requestCount` / `failureCount`
  - `recordRequest()` / `recordFailure()`
  - `markHealthy()` / `reset()`
- `NodeConfig`
  - 数据成员：`id` / `endpoint` / `weight = 1`
  - `operator==(const NodeConfig&)`
- `PhysicalNode`
  - 数据成员：`config` / `status`
  - `explicit PhysicalNode(NodeConfig cfg)`
- `ConsistentHash`
  - `using HashFunc = std::function<uint32_t(const std::string&)>`
  - `ConsistentHash(size_t virtualNodes = 150, HashFunc hashFunc = nullptr)`
  - `addNode(const NodeConfig&)`
  - `removeNode(const std::string& nodeId)`
  - `getNode(const std::string& key) -> std::optional<NodeConfig>`
  - `getHealthyNode(const std::string& key, size_t maxRetries = 3) -> std::optional<NodeConfig>`
  - `getNodes(const std::string& key, size_t count) -> std::vector<NodeConfig>`
  - `markUnhealthy(const std::string&)` / `markHealthy(const std::string&)`
  - `getAllNodes() -> std::vector<NodeConfig>`
  - `nodeCount()` / `virtualNodeCount()` / `empty()` / `clear()`
- 语义：当前公开头里没有 `getNodeStatus()`；状态相关检索应落到 `NodeStatus`、`PhysicalNode` 以及 `markHealthy()` / `markUnhealthy()`

### `BloomFilter<T, Hash>`

- `BloomFilter(size_t bitCount, Hash hash = Hash{})`
- `static fromExpectedItems(size_t expectedItems, double falsePositiveRate, Hash hash = Hash{}) -> BloomFilter`
- `static bitCountForExpectedItems(size_t expectedItems, double falsePositiveRate) -> size_t`
- `add(const T&)` / `addHash(uint64_t hash64)`
- `possiblyContains(const T&) const` / `possiblyContainsHash(uint64_t hash64) const`
- `clear()`
- `bitCount()` / `blockCount()` / `hashCount()` / `empty()` / `insertionCount()`
- 语义：
  - 采用 split-block Bloom Filter：每个 256-bit block 含 8 个 `uint32_t` word，每次 add/query 只访问一个 block，并在每个 word 中设置或检查 1 个 bit
  - `possiblyContains(...) == false` 表示一定不存在
  - `possiblyContains(...) == true` 只表示可能存在，存在假阳性；需要精确判断时必须回源确认
  - 不支持删除；普通 Bloom Filter 无法安全删除单个元素
  - false positive rate 受 bit 数、插入规模和 hash 分布影响，`fromExpectedItems(...)` 是容量估算而不是误判率承诺
  - 非线程安全；并发 add/query/clear 同一个实例时必须外部同步
  - 默认 `std::hash` 不保证跨进程或跨版本稳定；持久化或跨服务共享时应使用稳定 64-bit hash 并调用 `addHash()` / `possiblyContainsHash()`

### `TrieTree`

- `add`
- `contains`
- `startsWith`
- `query`
- `remove`
- `getWordsWithPrefix`
- `getAllWords`
- `size()` / `empty()` / `clear()`

### `MVCC`

- `using Version = uint64_t`
- `VersionedValue<T>`
  - 数据成员：`version` / `value` / `deleted`
  - `VersionedValue(Version v, std::unique_ptr<T> val, bool del = false)`
- `Mvcc<T>`
  - `Mvcc()`
  - `getValue(Version version) const -> const T*`
  - `getCurrentValue() const -> const T*`
  - `getValueWithVersion(Version version) const -> std::pair<const T*, Version>`
  - `putValue(std::unique_ptr<T>) -> Version`
  - `putValue(const T&) -> Version`
  - `updateValue(std::function<std::unique_ptr<T>(const T*)>) -> Version`
  - `compareAndSwap(Version expectedVersion, std::unique_ptr<T> newValue) -> Version`
  - `removeValue(Version version)` / `deleteValue() -> Version`
  - `isValid(Version version) const`
  - `currentVersion() const` / `versionCount() const`
  - `gc(size_t keepVersions)` / `gcOlderThan(Version olderThan)`
  - `getAllVersions() const -> std::vector<Version>`
  - `clear()`
- `Snapshot`
  - `explicit Snapshot(Version version)`
  - `version() const`
  - `read(const Mvcc<T>& mvcc) const -> const T*`
- `Transaction<T>`
  - `explicit Transaction(Mvcc<T>& mvcc)`
  - `read() const -> const T*`
  - `write(std::unique_ptr<T> value)`
  - `commit()`
  - `isCommitted() const`
- 语义：`compareAndSwap(...)` 冲突时返回 `0`；`deleteValue()` 会写入 tombstone 版本，而不是立即擦除历史版本

## 6. 应用与系统集成

| 模块 | 头文件 | 主要类型 |
|---|---|---|
| App | `galay-utils/app/app.hpp` | `CliErrorCode`、`CliError`、`CliValue<T>`、`Opt<T>`、`Positional<T>`、`Cmd`、`App` |
| Parser | `galay-utils/config/parser_manager.hpp` | `ParserBase`、`ConfigParser`、`IniParser`、`EnvParser`、`TomlParser`、`ParserManager` |
| Process | `galay-utils/system/process.hpp` | `ProcessId`、`ExitStatus`、`ProcessPriorityError`、`ProcessAffinityError`、`Process` |

### `App`

- 头文件拆分：`app/error.hpp`（错误类型）、`app/value.hpp`（无异常类型转换）、`app/arg.hpp`（选项）、`app/positional.hpp`（位置参数）、`app/cmd.hpp`（命令与解析）、`app/app.hpp`（入口，聚合以上全部）
- `CliErrorCode`
  - `UnknownOption` / `MissingValue` / `InvalidValue` / `MissingRequired` / `NotInChoices` / `UnknownSubcommand` / `HelpRequested` / `VersionRequested`
- `cliErrorString(CliErrorCode) -> const char*`
- `CliError`
  - `code` / `source` / `detail`
  - `isTermination()`：`HelpRequested` 与 `VersionRequested` 为正常终止
  - `message()`
- `CliValue<T>`（转换器，支持 `bool`、整数、浮点、`std::string`）
  - `typeName()` / `parse(std::string_view) -> std::expected<T, std::string>` / `toString(const T&)`
- `Opt<T>`
  - `def(T)` / `required(bool = true)` / `multi(bool = true)` / `choices(std::vector<std::string>)`
  - `bind(T*)` / `bindAll(std::vector<T>*)`
  - `value()` / `values()` / `isSet()` / `name()` / `shortName()` / `description()`
- `Positional<T>`
  - `def(T)` / `required(bool = true)` / `many(bool = true)` / `choices(...)`
  - `bind(T*)` / `bindAll(std::vector<T>*)`
  - `value()` / `values()` / `isSet()`
- `Cmd`
  - `opt<T>(name, shortName, description)` / `opt<T>(name, description)`
  - `flag(name, shortName, description)` / `flag(name, description)`
  - `pos<T>(name, description)`
  - `sub(name, description)` / `on(CmdCallback)`
  - `has(name)` / `rest()` / `selected()`
  - `name()` / `description()` / `printHelp(std::ostream&)`
- `App`
  - `App(std::string name, std::string description = "")`
  - `version(std::string)`：声明后 `--version` 生效
  - `run(int argc, const char* const* argv, std::ostream& out = std::cout, std::ostream& err = std::cerr) -> int`
- 语义：全程零异常；`--opt=value`、`-o value`、`-ovalue`、短选项合并、`--no-flag` 取反、`--` 之后全部按位置参数处理；`multi()` / `many()` 累积全部取值；解析失败时 `run()` 输出错误与帮助并返回 1，帮助/版本返回 0；同一 `App` 可重复解析，每次解析前自动重置状态
- 子命令优先级：非选项 token 先匹配子命令名，未命中再交给位置参数；因此根命令即使定义了 `many()` 位置参数，子命令依然可达
- 帮助优先：`--help` / `-h` 出现在 `--` 之前时优先于必选校验，`app sub --help` 输出的是子命令帮助而非父命令缺参错误
- 帮助输出顺序与声明顺序一致，左列自动对齐；标志位默认为假时不显示 `(default: ...)`

### `Parser`

- `ParserBase`
  - `parseFile`
  - `parseString`
  - `getValue`
  - `hasKey`
  - `getKeys`
  - `getValueAs<T>`
  - `lastError()`
- `ConfigParser`
  - `getKeysInSection`
  - `getArray`
- `IniParser`
  - 继承 `ConfigParser`
- `EnvParser`
  - 继承 `ParserBase`
- `TomlParser`
  - 继承 `ParserBase`
  - 支持基础 key-value、section、dotted key、字符串、数字、布尔值和数组
  - `getArray`
- `ParserManager`
  - `instance()`
  - `registerParser(extension, creator)`
  - `createParser(path)`
  - 默认注册：`.conf` → `ConfigParser`，`.ini` → `IniParser`，`.env` → `EnvParser`，`.toml` → `TomlParser`

### `Process`

- `ProcessId`
  - Windows：`DWORD`
  - POSIX：`pid_t`
- `ExitStatus`
  - 数据成员：`code` / `signaled` / `signal`
  - `success() const`
- `ProcessPriorityError`
- `processPriorityErrorString(ProcessPriorityError)`
- `ProcessAffinityError`
- `processAffinityErrorString(ProcessAffinityError)`
- `Process`
  - `Process::currentId()`
  - `Process::parentId()`
  - `Process::priority() -> std::expected<int, ProcessPriorityError>`
  - `Process::priority(ProcessId pid) -> std::expected<int, ProcessPriorityError>`
  - `Process::setPriority(int value) -> std::expected<void, ProcessPriorityError>`
  - `Process::setPriority(ProcessId pid, int value) -> std::expected<void, ProcessPriorityError>`
  - `Process::cpuAffinity() -> std::expected<std::vector<unsigned int>, ProcessAffinityError>`
  - `Process::cpuAffinity(ProcessId pid) -> std::expected<std::vector<unsigned int>, ProcessAffinityError>`
  - `Process::setCpuAffinity(std::span<const unsigned int> cpus) -> std::expected<void, ProcessAffinityError>`
  - `Process::setCpuAffinity(ProcessId pid, std::span<const unsigned int> cpus) -> std::expected<void, ProcessAffinityError>`
  - `wait(ProcessId pid, int options = 0) -> std::optional<ExitStatus>`
  - `spawn(const std::string& path, const std::vector<std::string>& args) -> ProcessId`
  - `execute(const std::string& command) -> ExitStatus`
  - `executeWithOutput(const std::string& command) -> std::pair<ExitStatus, std::string>`
  - `kill(ProcessId pid, int signal)`
  - `Process::isRunning(ProcessId pid)`
  - `daemonize()`

## 7. 编解码、密码学与公共定义

| 头文件 | 主要类型 |
|---|---|
| `galay-utils/encoding/base64.hpp` | `Base64Util` |
| `galay-utils/encoding/huffman.hpp` | `HuffmanCode`、`HuffmanTable<T>`、`HuffmanEncoder<T>`、`HuffmanDecoder<T>`、`HuffmanBuilder<T>` |
| `galay-utils/crypto/md5.hpp` | `MD5Util` |
| `galay-utils/crypto/salt.hpp` | `SaltGenerator` |
| `galay-utils/crypto/hmac.hpp` | `SHA256`、`HMAC` |
| `galay-utils/common/defn.hpp` | 基础类型别名、`NonCopyable`、`NonMovable`、`Singleton<T>` |

### `Base64Util`

- `Base64Encode(const std::string&, bool url = false)`
- `Base64EncodePem(const std::string&)`
- `Base64EncodeMime(const std::string&)`
- `Base64Decode(const std::string&, bool remove_linebreaks = false)`
- `Base64Encode(const unsigned char*, size_t len, bool url = false)`
- C++17：`Base64EncodeView(std::string_view, bool = false)`、`Base64EncodePemView(std::string_view)`、`Base64EncodeMimeView(std::string_view)`、`Base64DecodeView(std::string_view, bool = false)`
- 语义：
  - `url = false` 使用标准 Base64 字母表 `+/`；`url = true` 使用 URL-safe 字母表 `-_`
  - `Base64EncodePem(...)` 会按每 64 个字符插入换行；`Base64EncodeMime(...)` 会按每 76 个字符插入换行
  - `Base64Decode(...)` / `Base64DecodeView(...)` 遇到非法字符会抛 `std::runtime_error`
  - `remove_linebreaks = true` 会先移除输入中的 `
` 再解码，适合处理 PEM / MIME 风格输出

### `Huffman`

- `HuffmanCode`
  - 数据成员：`code` / `length`
- `HuffmanTable<T>`
  - `HuffmanTable()`
  - `addCode(const T& symbol, uint32_t code, uint8_t length)`
  - `getCode(const T& symbol) const -> const HuffmanCode&`
  - `hasSymbol(const T& symbol) const`
  - `getSymbol(uint32_t code, uint8_t length) const -> const T&`
  - `tryGetSymbol(uint32_t code, uint8_t length, T& symbol) const`
  - `getSymbols() const -> std::vector<T>`
  - `size() const`
  - `clear()`
- `HuffmanEncoder<T>`
  - `explicit HuffmanEncoder(const HuffmanTable<T>& table)`
  - `encode(const T& symbol)`
  - `encode(const std::vector<T>& symbols)`
  - `finish() -> std::vector<uint8_t>`
  - `bitCount() const`
  - `reset()`
- `HuffmanDecoder<T>`
  - `HuffmanDecoder(const HuffmanTable<T>& table, uint8_t minCodeLen = 1, uint8_t maxCodeLen = 32)`
  - `decode(const std::vector<uint8_t>& data, size_t symbolCount = 0) -> std::vector<T>`
- `HuffmanBuilder<T>`
  - `build(const std::unordered_map<T, size_t>& frequencies) -> HuffmanTable<T>`
  - `buildFromData(const std::vector<T>& data) -> HuffmanTable<T>`
- 语义：`HuffmanTable<T>::getCode()` / `getSymbol()` 在缺失项上抛异常；`HuffmanDecoder<T>::decode()` 在超过 `maxCodeLen` 时抛 `std::runtime_error`

### `MD5Util`

- `MD5(const std::string&)` / `MD5(const unsigned char*, size_t)`
- `MD5Raw(const std::string&)` / `MD5Raw(const unsigned char*, size_t)`
- C++17：`MD5View(std::string_view)` / `MD5RawView(std::string_view)`
- 语义：`MD5(...)` / `MD5View(...)` 返回 32 字符小写十六进制字符串；`MD5Raw(...)` / `MD5RawView(...)` 返回 `std::array<uint8_t, 16>` 原始摘要字节

### `SaltGenerator`

- `generateHex(size_t length = 32)`
- `generateBase64(size_t length = 32)`
- `generateBytes(size_t length = 32)`
- `generateCustom(size_t length, const std::string& charset)`
- `generateSecureHex(size_t length = 32)`
- `generateSecureBase64(size_t length = 32)`
- `generateSecureBytes(size_t length = 32)`
- `generateBcryptSalt()`
- `generateTimestamped(size_t length = 32)`
- `isValidHex(const std::string&)` / `isValidBase64(const std::string&)`
- 语义：
  - `generateHex(length)` / `generateBase64(length)` / `generateBytes(length)` / `generateSecure*` 里的 `length` 表示“随机字节数”，不是最终字符串长度
  - 因而十六进制输出通常是 `2 * length` 个字符，Base64 输出通常接近 `4 * ceil(length / 3)` 个字符
  - `generateCustom(length, charset)` 的 `length` 才是最终输出字符数
  - `generateBcryptSalt()` 使用 16 个安全随机字节并输出 22 字符 bcrypt 风格 Base64 盐值

### `SHA256`

- `hash(const uint8_t* data, size_t length) -> std::array<uint8_t, 32>`
- `hashHex(const uint8_t* data, size_t length)`
- `hashHex(const std::string& data)`
- 语义：`hash(...)` 返回 32 字节原始摘要；`hashHex(...)` 返回 64 字符小写十六进制字符串

### `HMAC`

- `hmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t dataLen) -> std::array<uint8_t, 32>`
- `hmacSha256(const std::string& key, const std::string& data) -> std::array<uint8_t, 32>`
- `hmacSha256Hex(const std::string& key, const std::string& data)`
- 语义：`hmacSha256(...)` 返回 32 字节原始 HMAC；`hmacSha256Hex(...)` 返回 64 字符小写十六进制字符串

### `defn.hpp`

- 预处理宏：`GALAY_PLATFORM_*`、`GALAY_ARCH_*`、`GALAY_COMPILER_*`、`GALAY_LIKELY(x)`、`GALAY_UNLIKELY(x)`、`GALAY_FORCE_INLINE`、`GALAY_UNUSED(x)`
- 基础别名：`i8` / `i16` / `i32` / `i64`、`u8` / `u16` / `u32` / `u64`、`f32` / `f64`、`usize` / `isize`
- 指针与函数别名：`UniquePtr<T>`、`SharedPtr<T>`、`WeakPtr<T>`、`Func<T>`
- 字符串别名：`String` / `StringView`
- 基类：`NonCopyable`、`NonMovable`
- `Singleton<T>`
  - 继承：`NonCopyable`、`NonMovable`
  - `static T& instance()`

## 8. 已知 API 边界

- `.ini` 使用独立公开类型 `IniParser`；`.toml` 使用 `TomlParser`
- 文档中不再使用“API 索引”旧名；本页 canonical 标题为“API参考”
- benchmark target 通过 `BUILD_BENCHMARKS=ON` 显式构建，默认不进入普通构建或 CTest

## 9. 返回、线程与使用语义

- 当前仓库没有统一的 `expected` / 错误码基类；检索失败语义时必须回到对应头文件签名，而不能把整个仓库当成单一错误模型
- 纯工具类主路径集中在 `core/`、`encoding/`、`crypto/`、`common/`，它们主要回答“输入是什么、返回值是什么”
- 线程池、等待器与对象池位于 `concurrency/`，系统资源接口位于 `system/`；检索时要额外关注阻塞、等待与资源释放语义
- `ThreadPool::addTask(...)` 返回 `std::future<...>`，而 `execute(...)` 是 fire-and-forget 风格；两者不应混用为同一等待模型
- `ObjectPool<T>` 与 `BlockingObjectPool<T>` 不是同一组方法：前者是“可扩容 + 非阻塞取对象”，后者是“固定池 + 阻塞等待”
- `RateLimiter` 不再提供 `acquire(...)` awaitable；使用无锁同步非阻塞 `tryAcquire(...)` 获取结果
- `ConsistentHash` 当前没有 `getNodeStatus()` 公开 API；状态检索要从 `NodeStatus`、`PhysicalNode` 以及标记接口理解
- `App::run(...)`、`ParserBase::*`、`Process::*`、`System::*` 直接面向进程 / 文件系统 / 环境变量等外部状态，细节问题需要结合真实调用环境理解
- 资源生命周期主要集中在 `ThreadPool`、对象池、限流器、断路器与 `Process` 相关 API；纯字符串 / 哈希 / 编码工具通常是无状态或短生命周期值语义

## 10. 交叉验证入口

- 基础能力示例：`examples/include/e1_basic.cpp`
- import 示例：`examples/cpp/utils/mcpp/e1_basic_usage.cc`
- include / umbrella / 模块 smoke：`test/<area>/*_test.cpp`、`test/import_smoke.cpp`

## 11. 继续阅读

- [03-使用指南](03-使用指南.md)
- [04-示例代码](04-示例代码.md)
- [06-高级主题](06-高级主题.md)
