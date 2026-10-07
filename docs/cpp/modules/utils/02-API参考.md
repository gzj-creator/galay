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
- `galay-utils/thread/thread.hpp`
- `galay-utils/common/pool.hpp`
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
- `galay-utils/system/cpu.hpp`
- `galay-utils/system/numa.hpp`
- `galay-utils/system/memory.hpp`
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
| TypeName | `galay-utils/core/type_name.hpp` | `get_type_name<T>()`、`get_type_name(obj)`、`demangle_symbol()` |
| BackTrace | `galay-utils/system/backtrace.hpp` | `BackTrace` |
| Signal | `galay-utils/system/signal.hpp` | `SignalHandler` |
| CPU | `galay-utils/system/cpu.hpp` | 硬件并发度、在线 CPU ID、当前 CPU 快照与线程亲和性 |
| Numa | `galay-utils/system/numa.hpp` | 在线节点、CPU/节点拓扑、相对距离与允许节点 |
| Memory | `galay-utils/system/memory.hpp` | 基础页大小、当前线程默认 NUMA 策略与原生状态恢复 |

### `CPU`、`Numa` 与 `Memory`：系统资源与线程放置

三个细粒度头分别承担 CPU 信息与亲和性、NUMA 拓扑与节点约束、页大小与内存策略；总头和 `galay.utils` 模块统一导出这三个类型。`system/detail/sysfs.hpp` 仅为安装时必需的内部实现头，不是独立公开 API。

下表除 `count()` 外，返回值均为 `std::expected<T, std::error_code>`，其中 `T` 列出成功值的类型。

| 静态方法 / 类型 | 成功类型 | 契约 |
|---|---|---|
| `CPU::count() noexcept` | `unsigned` | 原样返回 `std::thread::hardware_concurrency()` 的硬件并发度提示，可能为 0 |
| `CPU::online_cpus()` | `std::vector<unsigned>` | 系统在线逻辑 CPU ID 快照，排序去重，不按调用线程的 cpuset 过滤 |
| `CPU::current_id()` | `unsigned` | 查询瞬间正在执行调用线程的 CPU ID，不保证后续不迁移 |
| `CPU::cpu_affinity()` | `std::vector<unsigned>` | 当前调用线程的有效 CPU ID 集合，按 ID 排序 |
| `CPU::bind_current_thread(std::span<const unsigned> cpus)` | `std::vector<unsigned>` | 设置当前线程，回读实际 CPU 集合；输入不能为空，重复 ID 合并 |
| `Numa::online_nodes()` | `std::vector<unsigned>` | 系统在线 NUMA 节点快照，不按 cpuset 过滤 |
| `Numa::node_of_cpu(unsigned cpu)` | `unsigned` | 在线 CPU 所属的在线 NUMA 节点；未找到该 CPU 为 `no_such_device` |
| `Numa::cpus_of_node(unsigned node)` | `std::vector<unsigned>` | 在线节点的在线 CPU ID 集合，不按线程 affinity 过滤；无 CPU 的节点成功返回空集 |
| `Numa::distance(unsigned from, unsigned to)` | `unsigned` | sysfs 报告的正整数相对距离，无时间/字节单位，不承诺对称或固定值 |
| `Numa::allowed_numa_nodes()` | `std::vector<unsigned>` | 当前线程所属 cpuset 允许使用的内存节点 |
| `Memory::page_size()` | `std::size_t` | 系统基础页大小，单位为字节，必须为正；不表示 huge page 大小 |
| `Memory::numa_policy()` | `Memory::PolicyState` | 当前线程的原生 mode（包括 flags）和原生 nodemask 中的节点集合 |
| `Memory::set_numa_policy(Policy policy, std::span<const unsigned> nodes)` | `void` | 枚举接口仅覆盖 `Default` / `Bind` / `Interleave` |
| `Memory::restore_numa_policy(const PolicyState& state)` | `void` | 将保存的原生 mode、flags 和 nodemask 原样传给内核，失败必须向上传播 |

`CPU::count()` 保留标准库提示语义，避免改变线程池/调度器的并发度推导。在线 CPU 集合、当前线程 affinity 集合与该提示是三个不同概念；需要可运行 CPU 数量时，检查 `cpu_affinity()` 后使用集合的 `.size()`。CPU ID 不保证连续，例如 `{0, 2}` 的数量是 2，但 CPU 2 仍是合法 ID，不能用 `cpu < count()` 校验，也不能把线程索引直接作为 CPU ID。

Linux 的 CPU 读取与设置共用 `CPU_ALLOC` 动态 mask，`sched_getaffinity` 返回容量不足的 `EINVAL` 时扩大探测；不再有固定 `CPU_SETSIZE` / 1024 上限，也不再公开 `CPU::kMaxCpus`。实现支持 `0 <= CPU ID < INT_MAX`，并受内核实际 mask 和可分配内存约束；sysfs 超出该编号范围时返回 `value_too_large`，不会截断结果。绑定时由内核决定 online/cpuset 交集，调用方须比较实际集合与请求；绑定后的回读失败不代表绑定未生效，不承诺事务式回滚。

NUMA 拓扑来自 `/sys/devices/system/node/online`、`nodeN/cpulist`、`nodeN/distance`；距离列按在线节点排序后的顺序对应，不能把节点 ID 当成数组下标。拓扑查询与 `allowed_numa_nodes()` 清楚区分，内存策略仍由 `Memory` 负责。节点 mask 保留 `Numa::kMaxNodes == Memory::kMaxNodes == 1024`，支持节点 ID 0–1023；拓扑在线列表超容量为 `value_too_large`，参数超范围为 `invalid_argument`，内核 mask 容量不足保留 `EINVAL`，不返回截断集合。

原生策略恢复契约：

- `PolicyState` 含 `int native_mode` 和 `std::vector<unsigned> nodes`。`MPOL_F_RELATIVE_NODES` 下 `nodes` 表示相对当前 cpuset 的索引，恢复时不转换为物理节点 ID；`MPOL_F_STATIC_NODES` 保留物理 ID。两种 flags 不能同时指定。
- `MPOL_DEFAULT` / `MPOL_LOCAL` 要求空集；`MPOL_BIND` / `MPOL_INTERLEAVE` 及系统头文件定义的其他非 preferred 原生模式要求非空集。`MPOL_PREFERRED` 允许非空集（内核选择最低 ID）或无 flags 的空集（Linux 回读规范化为 `MPOL_LOCAL`）。`MPOL_LOCAL` 和空集 preferred 不能附带 static/relative flags。
- 原生 mode 必须在编译时 Linux 头文件定义的模式范围内。剩余 flags 组合、内存节点有效性、cpuset 约束和运行内核版本由 `set_mempolicy` 校验，例如旧内核不支持某些 flags 时保留其 `EINVAL`；不降级成其他策略。
- cpuset、节点在线状态或权限在保存后发生变化，可能导致恢复失败；调用方必须检查恢复结果。枚举接口仍只提供明确支持的三个策略，恢复接口没有扩大枚举契约。

错误与平台行为：

| 情况 | 结果 |
|---|---|
| Linux CPU / NUMA 查询与控制 | 使用 sysfs / POSIX 系统调用；失败保留 `errno` 与 `std::generic_category()` |
| 非 Linux CPU 在线/当前 ID/亲和性、NUMA 与内存策略接口 | `operation_not_supported`；`CPU::count()` 仍可用 |
| `Memory::page_size()` | Linux/macOS 使用 `sysconf(_SC_PAGESIZE)`，失败保留 errno；非正数且无 errno 为 `io_error`。Windows 使用无错误返回值的 `GetSystemInfo`，零页大小为 `io_error`；其他平台不支持 |
| sysfs 缺失或读取失败 | 保留 `ENOENT`、`EACCES`、`EISDIR` 等错误，不通过外部命令或猜测拓扑补偿 |
| 非法节点、畸形列表/距离、空在线列表 | `invalid_argument`；空 `cpulist` 是合法的无 CPU 节点 |
| ID 列表超容量 / 数字解析溢出 | `value_too_large` / `result_out_of_range`，范围在展开前检查 |

以上查询都是同步快照，不缓存，不保证跨多次读取的 CPU 热插拔一致性；sysfs 读取会同步访问文件，适合启动配置或诊断，不宜放进协程调度热路径。Linux 新建线程继承创建者当时的 CPU mask 和内存策略，已有工作线程不会被隐式修改。内存策略只影响后续分配/首次触碰，不迁移已有页，不覆盖已有分配器缓存或显式 VMA 策略。没有引入 libnuma、hwloc 或新的链接依赖。

本轮没有增加 `Memory::info()`：系统总内存、可用内存、cgroup 限额与进程 RSS 的范围和单位契约须分别定义后再设计查询结构。

压测配置、示例及限制见 [05-性能测试：CPU / NUMA 启动控制](05-性能测试.md#2026-09-29cpu--numa-启动控制)。

### `StringUtils`

- `split(std::string_view, char)`
- `split(std::string_view, std::string_view)`
- `split_respect_quotes(std::string_view, char, char)`
- `join(const std::vector<std::string>&, std::string_view)`
- `trim` / `trim_left` / `trim_right`
- `to_lower` / `to_upper`
- `starts_with` / `ends_with` / `contains`
- `replace` / `replace_first`
- `count(char)` / `count(std::string_view)`
- `to_hex` / `from_hex` / `to_visible_hex`
- `is_integer` / `is_float` / `is_blank`
- `format(...)`
- `parse<T>(...)`
- `to_string(...)`
- 语义：
  - 纯静态工具，不持有共享状态，线程安全性由输入输出对象自身决定
  - `split(..., "")` 返回原字符串；连续分隔符会保留空字段
  - `split_respect_quotes(...)` 只按 quote 状态忽略分隔符，不负责校验 quote 是否成对
  - `to_hex(nullptr, *)`、`to_visible_hex(nullptr, *)`、奇数长度或包含非法字符的 `from_hex(...)` 返回空结果
  - `parse<T>(...)` 要求去除首尾空白后完整解析；溢出、空串或尾随非法字符返回默认值

### `RandomGenerator` / `Randomizer`

- `RandomGenerator()`
- `explicit RandomGenerator(uint64_t seedValue)`
- `RandomGenerator::seed()` / `RandomGenerator::reseed()`
- `static Randomizer& instance()`
- `random_int` / `random_uint32` / `random_uint64`
- `random_double` / `random_float` / `random_bool`
- `random_string` / `random_hex` / `random_bytes`
- `uuid()`
- `seed()` / `reseed()`
- 语义：
  - `RandomGenerator` 是本地无锁生成器，非线程安全；共享同一个实例时必须由调用方外部加锁
  - `Randomizer` 是线程安全单例，内部用 mutex 保护共享随机引擎；可跨线程共享，但不适合协程热路径高频调用
  - 整数随机返回闭区间 `[min, max]`；浮点随机返回半开区间 `[min, max)`；`min >= max` 时返回 `min`
  - `random_bool(probability)` 对概率做边界处理：`<= 0` 返回 `false`，`>= 1` 返回 `true`
  - `random_string(0, *)`、`random_string(*, "")`、`random_hex(0)` 返回空字符串；`random_bytes(nullptr, *)` 为 no-op
  - `uuid()` 生成 RFC 4122 version 4 形态字符串，variant 位落在 `8`/`9`/`a`/`b`

### `Time`

- `Time::current_time_ms()` / `Time::current_time_us()` / `Time::current_time_ns()`
- `Time::format_time(std::time_t timestamp, const char* format, bool utc = false)`
- `Time::current_gmt_time(const char* format = "%a, %d %b %Y %H:%M:%S GMT")`
- `Time::current_local_time(const char* format = "%Y-%m-%d %H:%M:%S")`
- `StopWatch<Clock>`
  - `StopWatch()` / `explicit StopWatch(time_point start)`
  - `reset()`
  - `elapsed()` / `elapsed_ms()`
  - `start_time()`
- `Deadline<Clock>`
  - `explicit Deadline(time_point deadline)`
  - `from_now(duration)`
  - `expired()` / `remaining()`
  - `deadline_time()`
- `Backoff`
  - `Backoff::fixed(duration)` / `Backoff::linear(initial, step, max)` / `Backoff::exponential(initial, multiplier, max)`
  - `next()` / `reset()`
  - `attempts()` / `strategy()`
- 语义：
  - `System` 不再提供 `current_time*`、`current_gmt_time`、`current_local_time` 或 `format_time`；时间相关能力统一使用 `Time`
  - `format_time(...)` 的 `format == nullptr`、空格式、平台时间转换失败或格式化结果写入失败时返回空字符串
  - `StopWatch`、`Deadline`、`Backoff` 都是轻量非线程安全值对象，不创建线程，不提供 sleep 或调度语义
  - 这些类型不依赖平台、进程或 signal 头文件

### `System`

- 文件：`System::read_file` / `System::write_file` / `System::read_file_mmap`
- 文件系统：`System::file_exists` / `System::is_directory` / `System::file_size` / `System::create_directory` / `System::remove` / `System::list_directory`
- 网络：`System::resolve_host_ipv4` / `System::resolve_host_ipv6` / `System::check_address_type`
- 主机：`CPU::count()` / `System::hostname` / `System::current_dir` / `System::change_dir` / `System::executable_path`
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

- `template<typename T> get_type_name() -> std::string`
- `template<typename T> get_type_name(const T& obj) -> std::string`
- `demangle_symbol(const char* mangledName) -> std::string`
- 语义：GCC / Clang 下会尝试 demangle；失败或平台不支持时返回原始 `typeid(...).name()` / 符号名；`nullptr` 输入返回空字符串
  - 纯函数式工具，不持有共享可变状态

### `BackTrace`

- `get_stack_trace(int maxFrames = 64, int skipFrames = 1) -> std::vector<std::string>`
- `print_stack_trace(int maxFrames = 64, int skipFrames = 1)`
- `get_stack_trace_string(int maxFrames = 64, int skipFrames = 1) -> std::string`
- `install_crash_handlers()`
- 语义：
  - 当前只在 `__APPLE__` / `__linux__` 上真正采集堆栈；其他平台会返回空栈或只输出 `0 frames`
  - `print_stack_trace(...)` / `get_stack_trace_string(...)` 会在传入的 `skipFrames` 基础上再额外跳过 1 帧，用来隐藏包装函数自身
  - `install_crash_handlers()` 会安装 `SIGSEGV` / `SIGABRT` / `SIGFPE` / `SIGILL`，在支持的平台上额外安装 `SIGBUS`
  - crash handler 打印堆栈后会把该 signal 的处理方式恢复为 `SIG_DFL`，再重新 `raise(signal)`，因此进程仍会按默认方式终止

### `SignalHandler`

- `using Handler = std::function<void(int)>`
- `static SignalHandler& instance()`
- `bool set_handler(int signal, Handler handler)`
- `template<int... Signals> bool set_handler(Handler handler)`
- `bool remove_handler(int signal)` / `bool restore_default(int signal)`
- `bool ignore_signal(int signal)`
- `bool block_signal(int signal)` / `bool unblock_signal(int signal)`
- `bool has_handler(int signal) const`
- 语义：
  - `set_handler(...)` / `remove_handler(...)` / `restore_default(...)` / `ignore_signal(...)` / `block_signal(...)` / `unblock_signal(...)` 都返回 `bool`
  - Windows 下 `set_handler(...)` / `remove_handler(...)` / `ignore_signal(...)` 走 `std::signal(...)`；`block_signal()` / `unblock_signal()` 固定返回 `false`
  - POSIX 下 `set_handler(...)` 使用 `sigaction(..., SA_RESTART, ...)` 注册进程级 signal handler
  - `block_signal(...)` / `unblock_signal(...)` 在 POSIX 下通过 `pthread_sigmask(...)` 修改的是当前线程的 signal mask，而不是全局进程 mask

## 3. 并发、缓存与缓冲

| 模块 | 头文件 | 主要类型 |
|---|---|---|
| Cache | `galay-utils/cache/lru_cache.hpp` | `LruCache<Key, Value, Hash, KeyEqual, Clock, EnableStats>` |
| Bytes | `galay-utils/buffer/bytes.hpp` | `Bytes`、`ByteMetaData` |
| ByteQueueView | `galay-utils/buffer/byte_queue_view.hpp` | `ByteQueueView` |
| RingBuffer | `galay-utils/buffer/ring_buffer.hpp` | `RingBuffer` |
| Thread | `galay-utils/thread/thread.hpp` | `ThreadPool`、`TaskWaiter` |
| Pool | `galay-utils/common/pool.hpp` | `PoolableObject`、`ObjectPool<T>`、`BlockingObjectPool<T>` |

### `LruCache`

- 模板参数：`Key`、`Value`、`Hash = std::hash<Key>`、`KeyEqual = std::equal_to<Key>`、`Clock = std::chrono::steady_clock`、`EnableStats = false`
- 类型：
  - `EvictReason`：`Capacity` / `Expired` / `Removed` / `Cleared`
  - `ExpirationPolicy`：`ExpireAfterWrite` / `ExpireAfterAccess`
  - `Stats`：`hits` / `misses` / `inserts` / `updates` / `capacityEvictions` / `expiredEvictions` / `removes` / `clears`
  - `EvictCallback = std::function<void(const Key&, const Value&, EvictReason)>`
- 构造：
  - `LruCache(size_type capacity = 0, std::optional<duration> default_ttl = std::nullopt, EvictCallback on_evict = nullptr, ExpirationPolicy expirationPolicy = ExpirationPolicy::ExpireAfterWrite)`
  - `LruCache(size_type capacity, chrono duration default_ttl, EvictCallback on_evict = nullptr, ExpirationPolicy expirationPolicy = ExpirationPolicy::ExpireAfterWrite)`
- 写入：`put` / `put_for` / `put_until` / `emplace` / `emplace_for`
- 查询：`get` / `peek` / `contains`
- 管理：`remove` / `clear` / `size` / `empty` / `capacity` / `set_capacity` / `default_ttl` / `set_default_ttl` / `purge_expired`
- 哈希表调优：`reserve(size_type)` / `max_load_factor(float)` / `max_load_factor()`
- 统计：`stats_enabled()` / `stats()` / `reset_stats()`
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
- `malloc_bytes(size_t)` / `deep_copy_bytes(const ByteMetaData&)`
- `realloc_bytes(ByteMetaData&, size_t)` / `clear_bytes(ByteMetaData&)` / `free_bytes(ByteMetaData&)`

`Bytes`：

- move-only：支持移动构造和移动赋值，不支持拷贝
- 构造：`Bytes()` / `Bytes(std::string&)` / `Bytes(std::string&&)` / `Bytes(const char*)` / `Bytes(const uint8_t*)`
- 构造：`Bytes(const char*, size_t)` / `Bytes(const uint8_t*, size_t)` / `explicit Bytes(size_t capacity)`
- 非拥有视图：`Bytes::from_string(std::string&)` / `Bytes::from_string(std::string_view)` / `Bytes::from_c_string(const char*, size_t, size_t)`
- 查询：`data()` / `c_str()` / `size()` / `capacity()` / `empty()`
- 转换：`to_string()` / `to_string_view()`
- 管理：`clear()`
- 比较：`operator==` / `operator!=`
- 语义：
  - owning 构造函数会深拷贝输入字节；`Bytes` 析构或 `clear()` 时释放拥有的内存
  - `from_string(...)` / `from_c_string(...)` 只创建 non-owning 视图，调用方必须保证底层存储生命周期长于 `Bytes`
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
- 视图：`write_spans(std::array<std::span<std::byte>, 2>&)` / `read_spans(std::array<std::span<const std::byte>, 2>&)`
- POSIX I/O 视图：`get_write_iovecs(...)` / `get_read_iovecs(...)`
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
- `add_task(F&&, Args&&...) -> std::future<std::invoke_result_t<F, Args...>>`
- `execute(F&&)`
- `thread_count()` / `pending_tasks()` / `is_stopped()`
- `wait_all()`
- `stop()` / `stop_now()`
- 语义：`add_task(...)` 在池已停止时抛 `std::runtime_error`；`execute(...)` 只派发任务，不返回 `future`
- 实现：任务队列基于 `moodycamel::BlockingConcurrentQueue<std::function<void()>>`；提交路径不使用 `std::mutex` / `std::condition_variable`
- 阻塞：`wait_all()`、`stop()`、`stop_now()` 仍会阻塞调用线程，不是 coroutine awaitable

### `TaskWaiter`

- `add_task(ThreadPool&, F&&)`
- `wait()`
- `wait_for(timeout)`
- 实现：使用原子计数和 `atomic::wait/notify_all` 等待，不再使用 mutex/condition_variable

### `PoolableObject` / `IsPoolable<T>`

- `virtual ~PoolableObject() = default`
- `virtual void reset()`
- `IsPoolable<T>`：`T` 继承 `PoolableObject`，或自行提供 `reset() -> void`

### `ObjectPool<T>`

- `using Ptr = std::unique_ptr<T, std::function<void(T*)>>`
- `using Creator = std::function<T*()>`
- `using Destroyer = std::function<void(T*)>`
- `ObjectPool(size_t initialSize = 0, size_t max_size = 0, Creator creator = nullptr, Destroyer destroyer = nullptr)`
- `acquire()`：优先复用池内对象；池空时按需新建
- `try_acquire()`：仅在池内已有对象时成功，否则返回空 `Ptr`
- `size()` / `total_created()` / `empty()`
- `clear()` / `shrink(size_t targetSize)`

### `BlockingObjectPool<T>`

- `using Ptr = std::unique_ptr<T, std::function<void(T*)>>`
- `using Creator = std::function<T*()>`
- `using Destroyer = std::function<void(T*)>`
- `BlockingObjectPool(size_t poolSize, Creator creator = nullptr, Destroyer destroyer = nullptr)`
- `acquire()`：阻塞直到池内有对象可取
- `try_acquire_for(timeout)`：超时返回空 `Ptr`
- `available()`
- 语义：这是固定大小阻塞池；没有 `try_acquire()`、`total_created()`、`clear()`、`shrink()` 这组 API

## 4. 流控与容错

| 模块 | 头文件 | 主要类型 |
|---|---|---|
| RateLimiter | `galay-utils/resilience/rate_limiter.hpp` | `CountingSemaphore`、`TokenBucketLimiter`、`SlidingWindowLimiter`、`LeakyBucketLimiter` |
| CircuitBreaker | `galay-utils/resilience/circuit_breaker.hpp` | `CircuitState`、`CircuitBreakerError`、`CircuitBreakerExpected`、`CircuitBreakerConfig`、`BasicCircuitBreaker`、`CircuitBreaker` |

### `RateLimiter`

`rate_limiter.hpp` 的公开面分为四个无锁同步非阻塞限流器类型；异步 `acquire()` / awaitable 路径已移除，避免引入 `galay-kernel`。`try_acquire(...)` 成功返回 `true`，未通过限流直接返回 `false`：

- `CountingSemaphore`
  - `try_acquire(size_t n = 1)`
  - `release(size_t n = 1)`
  - `available()`
- `TokenBucketLimiter`
  - `TokenBucketLimiter(double rate, size_t capacity)`
  - `try_acquire(size_t tokens = 1)`
  - `available_tokens()`
  - `set_rate(double)` / `set_capacity(size_t)`
  - `rate()` / `capacity()`
- `SlidingWindowLimiter`
  - `SlidingWindowLimiter(size_t max_requests, std::chrono::milliseconds window_size)`
  - `try_acquire()`
  - `max_requests()`
  - `window_size()`
- `LeakyBucketLimiter`
  - `LeakyBucketLimiter(double rate, size_t capacity)`
  - `try_acquire(size_t amount = 1)`
  - `current_water()`
  - `rate()` / `capacity()`

依赖边界：

- 该头文件仅依赖标准库
- 不再提供异步限流器，不再依赖 `galay-kernel`；`galay/thirdparty/concurrentqueue/moodycamel` 仅用于线程池任务队列
- 限流器内部使用原子状态与 CAS，不使用内部互斥锁；需要 coroutine awaitable 时由上层运行时适配

### `CircuitBreaker`

- `CircuitBreakerError`
  - `Open`：熔断器打开，`execute(F&&)` 未执行主函数
- `CircuitBreakerExpected<T>`：约束 `execute` / `execute_with_fallback` 接受的 expected-like 返回类型；函数需按值返回该类型，类型需要提供 `value_type`、`error_type` 和 `has_value()`
- `CircuitBreakerConfig`
  - `failureThreshold`
  - `successThreshold`
  - `halfOpenMaxRequests`
  - `resetTimeout`
- `BasicCircuitBreaker<ClockType = std::chrono::steady_clock>`：可注入时钟源的熔断器模板，适合确定性测试或自定义时间源
- `CircuitBreaker`
  - `using CircuitBreaker = BasicCircuitBreaker<>`
  - `allow_request()`
  - `on_success()` / `on_failure()`
  - `execute(F&&)`：主函数必须返回 expected-like 结果，且 `error_type` 可从 `CircuitBreakerError` 构造；成功结果记录成功，失败结果记录失败；熔断打开时返回包含 `CircuitBreakerError::Open` 的失败结果
  - `execute_with_fallback(F&&, Fallback&&)`：主函数和 fallback 必须返回同一 expected-like 类型；主函数失败或熔断打开时返回 fallback 结果
  - `state()` / `state_string()`
  - `failure_count()` / `success_count()`
  - `reset()` / `force_open()`
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

- `hash32(const void*, size_t, uint32_t seed = 0)` / `hash32(const std::string&, uint32_t seed = 0)`
- `hash128(const void*, size_t, uint32_t seed = 0)` / `hash128(const std::string&, uint32_t seed = 0)`
- `hash128_raw(const void*, size_t, uint32_t seed = 0)` / `hash128_raw(const std::string&, uint32_t seed = 0)`
- C++17：`hash32_view(std::string_view, uint32_t seed = 0)`、`hash128_view(std::string_view, uint32_t seed = 0)`、`hash128_raw_view(std::string_view, uint32_t seed = 0)`
- 语义：`hash32(...)` 返回 32 位整数；`hash128(...)` / `hash128_view(...)` 返回 32 字符十六进制字符串；`hash128_raw(...)` / `hash128_raw_view(...)` 返回 `std::array<uint64_t, 2>`

### `Balancer`

- `RoundRobinLoadBalancer<T>`：`select()` / `size()` / `append(Type)`
- `WeightRoundRobinLoadBalancer<T>`：`select()` / `size()` / `append(Type, uint32_t)`
- `RandomLoadBalancer<T>`：`select()` / `size()` / `append(Type)`
- `WeightedRandomLoadBalancer<T>`：`select()` / `size()` / `append(Type, uint32_t)`

### `ConsistentHash`

- `NodeStatus`
  - 数据成员：`healthy` / `requestCount` / `failure_count`
  - `record_request()` / `record_failure()`
  - `mark_healthy()` / `reset()`
- `NodeConfig`
  - 数据成员：`id` / `endpoint` / `weight = 1`
  - `operator==(const NodeConfig&)`
- `PhysicalNode`
  - 数据成员：`config` / `status`
  - `explicit PhysicalNode(NodeConfig cfg)`
- `ConsistentHash`
  - `using HashFunc = std::function<uint32_t(const std::string&)>`
  - `ConsistentHash(size_t virtualNodes = 150, HashFunc hash_func = nullptr)`
  - `add_node(const NodeConfig&)`
  - `remove_node(const std::string& nodeId)`
  - `get_node(const std::string& key) -> std::optional<NodeConfig>`
  - `get_healthy_node(const std::string& key, size_t maxRetries = 3) -> std::optional<NodeConfig>`
  - `get_nodes(const std::string& key, size_t count) -> std::vector<NodeConfig>`
  - `mark_unhealthy(const std::string&)` / `mark_healthy(const std::string&)`
  - `get_all_nodes() -> std::vector<NodeConfig>`
  - `node_count()` / `virtual_node_count()` / `empty()` / `clear()`
- 语义：当前公开头里没有 `getNodeStatus()`；状态相关检索应落到 `NodeStatus`、`PhysicalNode` 以及 `mark_healthy()` / `mark_unhealthy()`

### `BloomFilter<T, Hash>`

- `BloomFilter(size_t bit_count, Hash hash = Hash{})`
- `static from_expected_items(size_t expectedItems, double falsePositiveRate, Hash hash = Hash{}) -> BloomFilter`
- `static bit_count_for_expected_items(size_t expectedItems, double falsePositiveRate) -> size_t`
- `add(const T&)` / `add_hash(uint64_t hash64)`
- `possibly_contains(const T&) const` / `possibly_contains_hash(uint64_t hash64) const`
- `clear()`
- `bit_count()` / `block_count()` / `hash_count()` / `empty()` / `insertion_count()`
- 语义：
  - 采用 split-block Bloom Filter：每个 256-bit block 含 8 个 `uint32_t` word，每次 add/query 只访问一个 block，并在每个 word 中设置或检查 1 个 bit
  - `possibly_contains(...) == false` 表示一定不存在
  - `possibly_contains(...) == true` 只表示可能存在，存在假阳性；需要精确判断时必须回源确认
  - 不支持删除；普通 Bloom Filter 无法安全删除单个元素
  - false positive rate 受 bit 数、插入规模和 hash 分布影响，`from_expected_items(...)` 是容量估算而不是误判率承诺
  - 非线程安全；并发 add/query/clear 同一个实例时必须外部同步
  - 默认 `std::hash` 不保证跨进程或跨版本稳定；持久化或跨服务共享时应使用稳定 64-bit hash 并调用 `add_hash()` / `possibly_contains_hash()`

### `TrieTree`

- `add`
- `contains`
- `starts_with`
- `query`
- `remove`
- `get_words_with_prefix`
- `get_all_words`
- `size()` / `empty()` / `clear()`

### `MVCC`

- `using Version = uint64_t`
- `VersionedValue<T>`
  - 数据成员：`version` / `value` / `deleted`
  - `VersionedValue(Version v, std::unique_ptr<T> val, bool del = false)`
- `Mvcc<T>`
  - `Mvcc()`
  - `get_value(Version version) const -> const T*`
  - `get_current_value() const -> const T*`
  - `get_value_with_version(Version version) const -> std::pair<const T*, Version>`
  - `put_value(std::unique_ptr<T>) -> Version`
  - `put_value(const T&) -> Version`
  - `update_value(std::function<std::unique_ptr<T>(const T*)>) -> Version`
  - `compare_and_swap(Version expectedVersion, std::unique_ptr<T> newValue) -> Version`
  - `remove_value(Version version)` / `delete_value() -> Version`
  - `is_valid(Version version) const`
  - `current_version() const` / `version_count() const`
  - `gc(size_t keepVersions)` / `gc_older_than(Version olderThan)`
  - `get_all_versions() const -> std::vector<Version>`
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
  - `is_committed() const`
- 语义：`compare_and_swap(...)` 冲突时返回 `0`；`delete_value()` 会写入 tombstone 版本，而不是立即擦除历史版本

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
- `cli_error_string(CliErrorCode) -> const char*`
- `CliError`
  - `code` / `source` / `detail`
  - `is_termination()`：`HelpRequested` 与 `VersionRequested` 为正常终止
  - `message()`
- `CliValue<T>`（转换器，支持 `bool`、整数、浮点、`std::string`）
  - `type_name()` / `parse(std::string_view) -> std::expected<T, std::string>` / `to_string(const T&)`
- `Opt<T>`
  - `def(T)` / `required(bool = true)` / `multi(bool = true)` / `choices(std::vector<std::string>)`
  - `bind(T*)` / `bind_all(std::vector<T>*)`
  - `value()` / `values()` / `is_set()` / `name()` / `short_name()` / `description()`
- `Positional<T>`
  - `def(T)` / `required(bool = true)` / `many(bool = true)` / `choices(...)`
  - `bind(T*)` / `bind_all(std::vector<T>*)`
  - `value()` / `values()` / `is_set()`
- `Cmd`
  - `opt<T>(name, short_name, description)` / `opt<T>(name, description)`
  - `flag(name, short_name, description)` / `flag(name, description)`
  - `pos<T>(name, description)`
  - `sub(name, description)` / `on(CmdCallback)`
  - `has(name)` / `rest()` / `selected()`
  - `name()` / `description()` / `print_help(std::ostream&)`
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
  - `parse_file`
  - `parse_string`
  - `get_value`
  - `has_key`
  - `get_keys`
  - `get_value_as<T>`
  - `last_error()`
- `ConfigParser`
  - `get_keys_in_section`
  - `get_array`
- `IniParser`
  - 继承 `ConfigParser`
- `EnvParser`
  - 继承 `ParserBase`
- `TomlParser`
  - 继承 `ParserBase`
  - 支持基础 key-value、section、dotted key、字符串、数字、布尔值和数组
  - `get_array`
- `ParserManager`
  - `instance()`
  - `register_parser(extension, creator)`
  - `create_parser(path)`
  - 默认注册：`.conf` → `ConfigParser`，`.ini` → `IniParser`，`.env` → `EnvParser`，`.toml` → `TomlParser`

### `Process`

- `ProcessId`
  - Windows：`DWORD`
  - POSIX：`pid_t`
- `ExitStatus`
  - 数据成员：`code` / `signaled` / `signal`
  - `success() const`
- `ProcessPriorityError`
- `process_priority_error_string(ProcessPriorityError)`
- `ProcessAffinityError`
- `process_affinity_error_string(ProcessAffinityError)`
- `Process`
  - `Process::current_id()`
  - `Process::parent_id()`
  - `Process::priority() -> std::expected<int, ProcessPriorityError>`
  - `Process::priority(ProcessId pid) -> std::expected<int, ProcessPriorityError>`
  - `Process::set_priority(int value) -> std::expected<void, ProcessPriorityError>`
  - `Process::set_priority(ProcessId pid, int value) -> std::expected<void, ProcessPriorityError>`
  - `Process::cpu_affinity() -> std::expected<std::vector<unsigned int>, ProcessAffinityError>`
  - `Process::cpu_affinity(ProcessId pid) -> std::expected<std::vector<unsigned int>, ProcessAffinityError>`
  - `Process::set_cpu_affinity(std::span<const unsigned int> cpus) -> std::expected<void, ProcessAffinityError>`
  - `Process::set_cpu_affinity(ProcessId pid, std::span<const unsigned int> cpus) -> std::expected<void, ProcessAffinityError>`
  - `wait(ProcessId pid, int options = 0) -> std::optional<ExitStatus>`
  - `spawn(const std::string& path, const std::vector<std::string>& args) -> ProcessId`
  - `execute(const std::string& command) -> ExitStatus`
  - `execute_with_output(const std::string& command) -> std::pair<ExitStatus, std::string>`
  - `kill(ProcessId pid, int signal)`
  - `Process::is_running(ProcessId pid)`
  - `daemonize()`

Linux 的进程亲和性接口操作传入 pid/tid 对应线程，默认使用进程主线程 pid；不会遍历已有线程。读取使用动态 mask，完整枚举实际 CPU ID；设置只做集合排序去重，编号与 online/cpuset 限制交给内核，不使用 `CPU::count()` 判定。Windows 保留 `GetProcessAffinityMask` / `SetProcessAffinityMask` 的现有单处理器组语义与 `DWORD_PTR` 位数限制。错误继续映射为现有 `ProcessAffinityError`；需要原始 `std::error_code` 的调用线程控制使用 `CPU` 接口。

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

- `base64_encode(const std::string&, bool url = false)`
- `base64_encode_pem(const std::string&)`
- `base64_encode_mime(const std::string&)`
- `base64_decode(const std::string&, bool remove_linebreaks = false)`
- `base64_encode(const unsigned char*, size_t len, bool url = false)`
- C++17：`base64_encode_view(std::string_view, bool = false)`、`base64_encode_pem_view(std::string_view)`、`base64_encode_mime_view(std::string_view)`、`base64_decode_view(std::string_view, bool = false)`
- 语义：
  - `url = false` 使用标准 Base64 字母表 `+/`；`url = true` 使用 URL-safe 字母表 `-_`
  - `base64_encode_pem(...)` 会按每 64 个字符插入换行；`base64_encode_mime(...)` 会按每 76 个字符插入换行
  - `base64_decode(...)` / `base64_decode_view(...)` 遇到非法字符会抛 `std::runtime_error`
  - `remove_linebreaks = true` 会先移除输入中的 `
` 再解码，适合处理 PEM / MIME 风格输出

### `Huffman`

- `HuffmanCode`
  - 数据成员：`code` / `length`
- `HuffmanTable<T>`
  - `HuffmanTable()`
  - `add_code(const T& symbol, uint32_t code, uint8_t length)`
  - `get_code(const T& symbol) const -> const HuffmanCode&`
  - `has_symbol(const T& symbol) const`
  - `get_symbol(uint32_t code, uint8_t length) const -> const T&`
  - `try_get_symbol(uint32_t code, uint8_t length, T& symbol) const`
  - `get_symbols() const -> std::vector<T>`
  - `size() const`
  - `clear()`
- `HuffmanEncoder<T>`
  - `explicit HuffmanEncoder(const HuffmanTable<T>& table)`
  - `encode(const T& symbol)`
  - `encode(const std::vector<T>& symbols)`
  - `finish() -> std::vector<uint8_t>`
  - `bit_count() const`
  - `reset()`
- `HuffmanDecoder<T>`
  - `HuffmanDecoder(const HuffmanTable<T>& table, uint8_t minCodeLen = 1, uint8_t maxCodeLen = 32)`
  - `decode(const std::vector<uint8_t>& data, size_t symbolCount = 0) -> std::vector<T>`
- `HuffmanBuilder<T>`
  - `build(const std::unordered_map<T, size_t>& frequencies) -> HuffmanTable<T>`
  - `build_from_data(const std::vector<T>& data) -> HuffmanTable<T>`
- 语义：`HuffmanTable<T>::get_code()` / `get_symbol()` 在缺失项上抛异常；`HuffmanDecoder<T>::decode()` 在超过 `maxCodeLen` 时抛 `std::runtime_error`

### `MD5Util`

- `MD5(const std::string&)` / `MD5(const unsigned char*, size_t)`
- `md5_raw(const std::string&)` / `md5_raw(const unsigned char*, size_t)`
- C++17：`md5_view(std::string_view)` / `md5_raw_view(std::string_view)`
- 语义：`MD5(...)` / `md5_view(...)` 返回 32 字符小写十六进制字符串；`md5_raw(...)` / `md5_raw_view(...)` 返回 `std::array<uint8_t, 16>` 原始摘要字节

### `SaltGenerator`

- `generate_hex(size_t length = 32)`
- `generate_base64(size_t length = 32)`
- `generate_bytes(size_t length = 32)`
- `generate_custom(size_t length, const std::string& charset)`
- `generate_secure_hex(size_t length = 32)`
- `generate_secure_base64(size_t length = 32)`
- `generate_secure_bytes(size_t length = 32)`
- `generate_bcrypt_salt()`
- `generate_timestamped(size_t length = 32)`
- `is_valid_hex(const std::string&)` / `is_valid_base64(const std::string&)`
- 语义：
  - `generate_hex(length)` / `generate_base64(length)` / `generate_bytes(length)` / `generateSecure*` 里的 `length` 表示“随机字节数”，不是最终字符串长度
  - 因而十六进制输出通常是 `2 * length` 个字符，Base64 输出通常接近 `4 * ceil(length / 3)` 个字符
  - `generate_custom(length, charset)` 的 `length` 才是最终输出字符数
  - `generate_bcrypt_salt()` 使用 16 个安全随机字节并输出 22 字符 bcrypt 风格 Base64 盐值

### `SHA256`

- `hash(const uint8_t* data, size_t length) -> std::array<uint8_t, 32>`
- `hash_hex(const uint8_t* data, size_t length)`
- `hash_hex(const std::string& data)`
- 语义：`hash(...)` 返回 32 字节原始摘要；`hash_hex(...)` 返回 64 字符小写十六进制字符串

### `HMAC`

- `hmac_sha256(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t dataLen) -> std::array<uint8_t, 32>`
- `hmac_sha256(const std::string& key, const std::string& data) -> std::array<uint8_t, 32>`
- `hmac_sha256_hex(const std::string& key, const std::string& data)`
- 语义：`hmac_sha256(...)` 返回 32 字节原始 HMAC；`hmac_sha256_hex(...)` 返回 64 字符小写十六进制字符串

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
- GCC 14 / libstdc++ 的 `import galay.utils;` 消费端仍存在仓库原有的 named-module 与标准头重复声明问题（如 `__cxa_init_primary_exception`、`std::nothrow`）；`galay_utils.cppm` 接口单元可独立编译。本轮系统接口验证不将该消费端问题当作接口错误。

## 9. 返回、线程与使用语义

- 当前仓库没有统一的 `expected` / 错误码基类；检索失败语义时必须回到对应头文件签名，而不能把整个仓库当成单一错误模型
- 纯工具类主路径集中在 `core/`、`encoding/`、`crypto/`、`common/`，它们主要回答“输入是什么、返回值是什么”
- 线程池位于 `thread/`，对象池位于 `common/`，系统资源接口位于 `system/`；检索时要额外关注阻塞、等待与资源释放语义
- `ThreadPool::add_task(...)` 返回 `std::future<...>`，而 `execute(...)` 是 fire-and-forget 风格；两者不应混用为同一等待模型
- `ObjectPool<T>` 与 `BlockingObjectPool<T>` 不是同一组方法：前者是“可扩容 + 非阻塞取对象”，后者是“固定池 + 阻塞等待”
- `RateLimiter` 不再提供 `acquire(...)` awaitable；使用无锁同步非阻塞 `try_acquire(...)` 获取结果
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
