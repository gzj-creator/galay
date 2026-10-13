---
doc_id: galay.utils.rdtsc
title: Galay Utils Rdtsc 硬件计数器
system: galay
module: galay-utils
doc_type: api
status: active
applies_to: v7.0.2 的 C++23 utils 实现；GCC/Clang x86-64 与 AArch64
updated_at: 2026-10-13
access_level: internal
---

# Galay Utils Rdtsc 硬件计数器

## 用途与前提

`galay::utils::Rdtsc` 提供低成本的硬件计数器读取，以及 tick 差值到纳秒的近似换算。适用于固定同一 CPU 的短区间 profiling、同步代码和微基准。x86-64 读取 TSC，AArch64 读取 ARM Generic Timer；ARM tick 是固定频率计时器单位，不是 CPU 核心周期。TSC 频率也不等于当前 CPU 主频。

头文件是 `galay/cpp/galay-utils/system/rdtsc.hpp`，显式初始化需链接 `galay::utils`。总头 `galay_utils.hpp` 和模块 `import galay.utils;` 导出相同类型。初始化后的读取和换算不加锁、不分配内存，不访问标准时钟。

计数器没有 Unix 或 `steady_clock` 纪元，不支持直接赋值给标准时钟的 `time_point`。调用方必须保持计数器访问权限，并保证测量期间不迁核、不改变计数频率。当前实现不承诺跨 CPU 同步、休眠恢复或虚拟机迁移后的连续性。`has_invariant_frequency()` 仅报告架构频率稳定标记，不能证明这些条件成立。

## 初始化与错误

`Rdtsc::create()` 返回 `std::expected<Rdtsc, std::error_code>`，对象不能默认构造。`is_supported` 只表示编译器、架构和 128 位整数支持；硬件或权限检查仍可能失败。函数不会在不支持时改用其他时钟。

在 x86-64 上，初始化检查 TSC、SSE2 与 RDTSCP。Linux 使用 `PR_GET_TSC` 检查当前线程的权限；其他读取线程也必须有权限。优先使用完整的 CPUID `0x15` 计数器/晶振比例。信息不完整时，以参考时钟校准五轮，每轮睡眠 20 ms，取频率中位数；Linux 参考源为 `CLOCK_MONOTONIC_RAW`，其他平台为 `steady_clock`。初始化可能阻塞约 100 ms，应在启动工作线程和协程之前完成。

校准检查起止 CPU ID、计数器回退、参考时间前进及五轮频率的离散范围。范围超过中位数的 0.5% 时返回错误；这个阈值只筛除明显不稳定样本，不是纳秒精度保证。校准没有禁止缺少 invariant 标记的硬件，使用者仍需限定适用环境。

AArch64 频率来自 `CNTFRQ_EL0`。调用方须先确认 OS/虚拟机允许用户态读取 `CNTFRQ_EL0` 和 `CNTVCT_EL0`；本实现没有信号探测逻辑，禁止访问时寄存器读取可能陷入系统，而不是返回 `expected` 错误。MSVC、32 位 ARM、PowerPC 与不具备 128 位整数的目标不提供此实现。

| 错误 | 触发条件 | 处理 |
|---|---|---|
| `operation_not_supported` | 编译目标或 x86 硬件能力不足，或架构频率为零 | 在调用方选择其他已验证计时方案 |
| `operation_not_permitted` | Linux 当前线程的 TSC 权限被禁用 | 修正权限后重新初始化 |
| `resource_unavailable_try_again` | 校准中迁核、回退、参考时间异常或频率离散超阈值 | 固定 CPU，排除干扰后在初始化阶段重试 |
| `value_too_large` | 定点参数或纳秒结果超出可表示范围 | 减小测量范围并检查单位 |
| 系统 `errno` | `prctl` 或参考时钟查询失败 | 保留 `error_code`，按系统原因诊断 |

## 读取与排序

| 方法 | x86-64 指令 | AArch64 指令 | 用途 |
|---|---|---|---|
| `read_relaxed()` | `RDTSC` | `MRS CNTVCT_EL0` | 最低成本读 tick，不保证与周围指令的顺序 |
| `read_start()` | `LFENCE; RDTSC; LFENCE` | `ISB; MRS CNTVCT_EL0; ISB` | 带屏障的测量起点 |
| `read_end()` | `RDTSCP; LFENCE` | 与 `read_start()` 相同 | 带屏障的测量终点 |

x86 路径要求 `LFENCE` 具备执行序列化语义，旧 AMD CPU 上该条件依赖 OS 配置；初始化不会检测这个配置。x86 起止读取等待前序指令和本地加载，不等待所有存储对其他核可见。ARM `ISB` 同步指令执行，不替代要求内存完成的 `DSB`。这些接口不是跨线程同步原语。

起止读取具有编译器 `memory` clobber，约束内存访问顺序，但不能阻止编译器移动无关的纯算术表达式。测量目标必须保留可观察结果，必要时使用成熟 benchmark 框架的数据依赖和优化屏障。空区间的测量包含读取和屏障本身的成本。

## 纳秒换算与精度

`frequency_hz()` 返回初始化时确定的计数频率，单位为 ticks/s（Hz），不是当前 CPU 核心主频。`frequency_source()` 返回 `RdtscFrequencySource`，枚举类型及每个枚举项均在头文件中提供中文 Doxygen 注释：

| 枚举值 | 频率获取方式 | 适用条件 |
|---|---|---|
| `RdtscFrequencySource::kArchitectural` | x86-64 CPUID `0x15` 的晶振频率与计数器比例，或 AArch64 `CNTFRQ_EL0` | 架构提供完整的计数频率信息 |
| `RdtscFrequencySource::kCalibrated` | 以参考时钟测量 tick 增量，取五轮频率中位数 | x86-64 架构频率信息不完整时使用 |

频率来源只说明获取方式；频率稳定标记由 `has_invariant_frequency()` 单独报告。`to_nanoseconds(ticks)` 接受起止差值，返回 `expected<chrono::nanoseconds, error_code>`。

初始化预计算 `floor((10^9 << 32) / frequency_hz)`，热路径使用 128 位整数乘法再右移 32 位，没有运行时除法。结果向下取整。相对给定频率的精确换算，定点截断少算的误差小于 `ticks / 2^32 + 1` ns；频率估计误差需要额外计算。较长区间误差会累积，不能将此接口当作经过 OS 校正的长期时钟。

调用方先检查 `end >= start`，再传入 `end - start`。返回值检测有符号 64 位纳秒结果溢出，不会自动检测 tick 回退、计数器回绕或频率变化。

## 完整调用示例

以下程序在 Linux 绑定当前线程到继承集合中的第一个 CPU，在初始化完成后测量字符串分割。非 Linux 平台需由调用方确认同一 CPU 和寄存器访问条件。编译时使用 C++23，并链接 `galay::utils`。

```cpp
#include <galay/cpp/galay-utils/common/string.hpp>
#include <galay/cpp/galay-utils/system/cpu.hpp>
#include <galay/cpp/galay-utils/system/rdtsc.hpp>
#include <iostream>
#include <span>

int main() {
#if defined(__linux__)
    const auto allowed = galay::utils::CPU::cpu_affinity();
    if (!allowed || allowed->empty()) {
        return 1;
    }
    const unsigned cpu = allowed->front();
    const auto pinned = galay::utils::CPU::bind_current_thread(std::span{&cpu, 1});
    if (!pinned || pinned->size() != 1 || pinned->front() != cpu) {
        return 1;
    }
#endif
    const auto counter = galay::utils::Rdtsc::create();
    if (!counter) {
        std::cerr << counter.error().message() << '\n';
        return 1;
    }
    const auto start = counter->read_start();
    const auto parts = galay::utils::StringUtils::split("a,b,c", ',');
    const auto end = counter->read_end();
    if (end < start) {
        return 1;
    }
    const auto elapsed = counter->to_nanoseconds(end - start);
    if (!elapsed) {
        std::cerr << elapsed.error().message() << '\n';
        return 1;
    }
    std::cout << "parts=" << parts.size() << " elapsed_ns=" << elapsed->count() << '\n';
    return 0;
}
```

## 构建与复测

从仓库根目录运行，要求 GCC/Clang、CMake 与 Ninja。该配置仅启用 utils、kernel 和本地 RPC benchmark，使用 epoll，不启动外部服务。Linux 示例假设 CPU 0/1 位于允许集合；先执行 `taskset -pc $$` 核对。

```bash
cmake --preset consumer-minimal -B build/rdtsc-eval -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DGALAY_BUILD_DEBUG=OFF \
  -DBUILD_TESTING=ON -DGALAY_BUILD_BENCHMARKS=ON \
  -DGALAY_BUILD_RPC=ON -DGALAY_BUILD_SERDE=OFF \
  -DGALAY_DISABLE_IOURING=ON -DGALAY_INSTALL_CPP23_MODULE_INTERFACES=OFF
cmake --build build/rdtsc-eval --target t27_rdtsc \
  benchmark_utils_time_sources benchmark_rpc_unary_mode_route_cache
ctest --test-dir build/rdtsc-eval --output-on-failure -R '^utils\.rdtsc$'
GALAY_BENCH_CPUS=0 build/rdtsc-eval/benchmark/cpp/utils/benchmark_utils_time_sources 2000000
GALAY_BENCH_CPUS=1 build/rdtsc-eval/benchmark/cpp/utils/benchmark_utils_time_sources 2000000
GALAY_BENCH_CPUS=0 build/rdtsc-eval/benchmark/cpp/rpc/benchmark_rpc_unary_mode_route_cache -n 2000000 --clock steady
GALAY_BENCH_CPUS=0 build/rdtsc-eval/benchmark/cpp/rpc/benchmark_rpc_unary_mode_route_cache -n 2000000 --clock rdtsc
```

`benchmark_utils_time_sources [iterations]` 默认每场景每轮 2,000,000 次，固定九轮，预热 100,000 次，奇偶轮反转场景顺序。原始输出含运行环境、频率、来源、每轮样本、校验和及中位数/极值。未固定单 CPU、初始化失败、Q32 或浮点差值场景的 tick 回退、Q32 换算失败时非零退出；所有 tick 相减前都检查 `end >= start`，裸读取对照不做逐读数单调性检测。该 benchmark 当前只在 Linux 核验 affinity，非 Linux 目标明确退出，不输出未经绑定验证的样本；这不改变 `Rdtsc` 类的编译平台范围。

RPC benchmark 新增 `--clock steady|rdtsc`，默认 `steady`；`-n` 默认 1,000,000，必须为正整数。`rdtsc` 模式在循环外选择专门的计时代码，以有序读取记录每次查找；外层吞吐统计仍用标准时钟。全部迭代都包含取时成本，只有前 10,000 次记录纳秒样本。必须交替两种模式，不能用一次运行宣称业务吞吐改善。

两个 benchmark 的 RDTSC 模式都在 Linux 回读单 CPU affinity；非 Linux 无法通过当前入口核验绑定，明确拒绝 RDTSC 测量。RPC 的 `steady` 模式仍可按公共 benchmark 环境入口的规则运行。这是测量入口的运行限制，不是 `Rdtsc` 对其他平台的隐式回退。

## 生产代码接入审查结论

适用范围：2026-10-12 当前工作区的 `src/cpp/` 生产实现，补充核对 `src/c/` 的单调时钟等待；不把 `test/`、`benchmark/`、第三方代码或生成的 module prelude 计入生产候选。**当前没有满足全部前提的生产计时路径，生产时间源保留原实现。** 这轮结论来自时间调用、时间字段及调用链的核对，不能将微基准的取时收益写成已完成的生产优化。

审查同时覆盖直接 `steady_clock::now()` / `system_clock::now()`、`RpcClock` / `SteadyClock` 别名、模板 `Clock::now()`、`Time::current_time_*()`、`clock_gettime()` 和 `std::time()`。随后检查 `time_point`、`started_at`、`m_start_time`、`m_last_used`、原子时间字段及 `wait_for` / `wait_until` 的消费者。可重复搜索命令和命中记录见 [生产时间调用盘点](evidence/rdtsc_production_time_inventory_2026-10-12.txt)。

### 生产时间源保留原因

以下路径均保留当前标准时钟；表中的符号可在链接的实现中独立定位。

| 生产路径与依据 | 当前消费者与时间语义 | 排除原因 |
|---|---|---|
| [Time / StopWatch / Deadline](../../../../src/cpp/galay-utils/system/time.hpp) 的 `Time::current_time_ms/us/ns()`、时间格式化 | Unix epoch 或本地/GMT 时间 | 原始 tick 没有墙钟纪元 |
| 同一文件的 `StopWatch<Clock>` | 指定 `time_point` 构造、`start_time()` 返回时钟时间点，`elapsed()` 直接返回 duration | 公开 chrono 契约没有注入实例或报告读取失败的接口；非线程安全也不等于禁止在线程之间顺序移动；`src/` 中没有本地短区间生产调用点 |
| 同一文件的 `Deadline<Clock>`；[kernel timer](../../../../src/cpp/galay-kernel/common/timer.hpp)、[timer manager](../../../../src/cpp/galay-kernel/common/timer_manager.hpp)、[多线程 timer manager](../../../../src/cpp/galay-kernel/common/timer_manager_mt.hpp) | 绝对到期时间、时间轮纪元及下一次唤醒间隔 | 参与调度和超时，不能使用仅保证同 CPU 区间的计数器 |
| [HTTP reader](../../../../src/cpp/galay-http/kernel/http_reader.h)、[HTTP/2 stream manager](../../../../src/cpp/galay-http2/kernel/stream_manager.h)、[HTTP/2 core](../../../../src/cpp/galay-http2/kernel/h2_core.h)、[RPC policy](../../../../src/cpp/galay-rpc/kernel/rpc_policy.h) 的 deadline/timer | 请求和帧超时、settings/ping、重试剩余时间与优雅关闭 | 需要与 OS 等待、协程 timeout 或公开时钟时间点保持一致 |
| [ThreadPool / TaskWaiter](../../../../src/cpp/galay-utils/thread/thread.hpp)、[BlockingObjectPool](../../../../src/cpp/galay-utils/common/pool.hpp)、[BlockingExecutor](../../../../src/cpp/galay-kernel/core/blocking_executor.cc) | 等待截止时间、condition variable 超时及工作线程保活 | 控制阻塞等待与资源生命周期，不是性能采样 |
| [LruCache](../../../../src/cpp/galay-utils/cache/lru_cache.hpp) 的 TTL / `put_until()` | 条目持久保存到期时间，过期堆和后续访问比较 `Clock::time_point` | 即便单线程使用，TTL 也跨越多个操作并有公开绝对时间接口；纯容量模式本来就不读取时钟 |
| [rate limiter](../../../../src/cpp/galay-utils/resilience/rate_limiter.hpp)、[circuit breaker](../../../../src/cpp/galay-utils/resilience/circuit_breaker.hpp)、[HTTP blacklist](../../../../src/cpp/galay-http/plugin/blacklist/blacklist.hpp) | 补充令牌、滑动窗口、熔断恢复、封禁截止时间和衰减基准 | 跨调用、跨线程的业务时间状态；不能将原始 tick 放入现有原子字段或 ConnInfo |
| [Redis connection pool](../../../../src/cpp/galay-redis/async/conn_pool.h)、[Redis topology](../../../../src/cpp/galay-redis/async/topology_client.cc) | `m_last_used`、空闲/健康检查时间及 `m_last_refresh_time` | 连接可交接，刷新和保活跨调用；属于资源时间状态 |
| [RPC channel](../../../../src/cpp/galay-rpc/kernel/rpc_channel.h) 的 `started_at` / `emit_metric()` | 请求起点经出站队列和 pending 传递，在 reader loop 完成时计算 latency | 起止线程不保证相同；duration 结果并不能消除起点跨线程的约束 |
| [Redis pool awaitable](../../../../src/cpp/galay-redis/details/pool_awaitable.inl) / [record_acquire_stats](../../../../src/cpp/galay-redis/async/conn_pool.cc) | 获取连接的起点跨越协程等待，在完成时累计毫秒统计 | Ready 分支局部同步，但同一统计入口也覆盖 Creating / Waiting；不能据此替换整个 `m_start_time` |
| [MongoClient::connect](../../../../src/cpp/galay-mongo/sync/mongo_client.cc) 的 `round_trip_time` | 同步 connect/hello 区间，结果用于 `select_server()` 选择节点 | 经过 [Connection::connect](../../../../src/cpp/galay-mongo/protoc/connection.cc) 的 `poll()` 和阻塞网络收发，区间不是局部短计算，也不是纯 profiling 样本 |
| [Span](../../../../src/cpp/galay-tracing/kernel/span.h)、[OTLP exporter](../../../../src/cpp/galay-tracing/kernel/otlp_http_exporter.cc)、[batch processor](../../../../src/cpp/galay-tracing/kernel/batch_span_processor.cc) | 公开起止/event `Clock::time_point`、导出器消费时间点、flush/shutdown deadline 与原子截止时间 | 对象可移动和跨线程交付，导出/调度契约不能接收无纪元 tick |
| [日志时间](../../../../src/cpp/galay-tracing/log/log_record.h)、[HTTP 文件时间](../../../../src/cpp/galay-http/server/http_etag.h)、[HTTP/2 文件时间](../../../../src/cpp/galay-http2/server/h2_static_file.cc)、[etcd cluster](../../../../src/cpp/galay-etcd/cluster/etcd_cluster_client.h)、[Salt](../../../../src/cpp/galay-utils/crypto/salt.hpp) 的 timestamp | 墙钟/文件时间或携带毫秒时间戳的输出 | 缺少 Unix epoch，不能替换；multipart 边界等墙钟调用也不在持续时间采样范围 |
| [tracing ID 的 fallback_word](../../../../src/cpp/galay-tracing/common/id_format.cc) | 时钟读数与原子计数混合生成标识符 | 没有起止区间和性能样本，运行时权限/初始化也无注入契约，不为低成本读数新增全局计数器 |
| [C coroutine timeout](../../../../src/c/galay-kernel-c/coro-c/coro_task.c)、[C bounded channel](../../../../src/c/galay-kernel-c/concurrency-c/bounded_channel.c)、[C async mutex](../../../../src/c/galay-kernel-c/async-c/async_mutex.c)、[C async waiter](../../../../src/c/galay-kernel-c/async-c/async_waiter.c) | `CLOCK_MONOTONIC` 毫秒截止时间与等待检查 | C 层仍是调度/等待语义，不新增 C++ 计数器桥接 |

### 为什么局部耗时结果仍不足以接入

RPC channel 在 `call_with_mode()` 中记录 `outbound.started_at` 并复制到 `pending_hint`，再经线程安全 `m_outbound` 队列交给 writer；`reader_loop()` 在响应完成后调用 `emit_metric()`。即使 metrics 对外只传 duration，原始起点也会跨越队列和挂起点。把两次读数换成 RDTSC 会依赖未承诺的跨 CPU 同步。现有 `RpcClock` deadline 也必须继续使用标准时钟。

Redis/Rediss 的 `PoolAcquireAwaitable::prepare_suspend()` 确实包含不挂起的 Ready 分支，但同一个 `m_start_time` 也被 Creating 和 Waiting 分支带到 `await_resume()`。只改 Ready 分支需要新增独立采样契约，并让调用方显式提供已经在读取线程验证的计数器；当前共享 pool 配置不提供该生命周期。不能以慢分支暗中切回 chrono 的方式扩大本次范围。

Mongo `round_trip_time` 的起止在同一个同步函数内，结果也只有 duration，但测量包含连接等待和 hello 网络往返，且用于节点选择。固定 CPU 不能把它变成短计算 profiling；现有连接流程没有明确启用的计时器或频率变化后的失败处理。单纯取时微基准不能证明改变节点选择计时安全或有实际收益。

### 后续接入必须具备的架构前提

本次不新增全局 Clock、单例、兼容包装、隐式 chrono 回退或没有消费者的 profiling 抽象。后续只有发现真实的局部同步采样需求时，才适合按以下条件重新评估：

1. 起止读数都留在同一个工作线程的一次同步调用内，不跨 `co_await`、任务提交、队列交接或阻塞网络等待。原始 tick 只保存在局部变量，对外输出 duration 或明确错误。
2. 应用在初始化阶段只创建一次 `Rdtsc` 并检查 `expected`，随后显式传给测量入口；启用方式和采样失败处理由 API 明确表达，不能依赖 chrono Clock 的静态 `now()` 隐藏初始化。
3. 读取线程在启动就绪之前验证单 CPU affinity 和本线程 TSC 权限，保持这些条件不变。Linux kernel scheduler 当前的 [apply_configured_affinity](../../../../src/cpp/galay-kernel/core/scheduler.cc) 结果在 [IO worker 启动](../../../../src/cpp/galay-kernel/core/io_scheduler_base.hpp) 和 [parallel worker 启动](../../../../src/cpp/galay-kernel/parallel/parallel_scheduler.cc) 中被忽略；仅设置 Runtime affinity 不构成已验证前提。AArch64 另需部署环境事先保证两个寄存器可读。
4. 目标环境证明频率/排序前提成立，并明确不覆盖迁核、休眠恢复和 VM 迁移。若无法证明，应明确拒绝启用，而不是在请求中重校准或偷偷切换时钟。
5. 先验证正常结果、初始化失败、读取线程权限和 affinity 错误等行为，再在同一二进制中按单 CPU 交替运行有序 RDTSC 与 steady 对照，报告多轮中位数和原始样本。

### 已具备条件的测量工具

当前安全用法仍集中在已有测量工具，不能算作生产时间源接入：

| 测量入口 | 已核实的边界 | 输出含义 |
|---|---|---|
| `benchmark/cpp/rpc/b7_unary_mode_route_cache.cc` 的 `--clock rdtsc` | main 初始化一次，Linux 回读单 CPU，同一线程同步查找，有序起止，差值/换算错误明确终止 | 计时开销对同步路由微基准吞吐的影响 |
| `benchmark/cpp/utils/b12_time_sources.cc` 的计时与 LRU 场景 | Linux 回读单 CPU，初始化失败即终止，本地 tick、显式换算，九轮反转顺序对照 | 原始读取、换算和逐操作采样成本；没有修改 LRU 的 TTL 或业务逻辑 |

生产路径没有发生行为变化，因此不新增生产行为测试，也没有生产 before/after 收益可以报告；已有 RDTSC 和 benchmark 的验证记录继续作为工具正确性与测量成本的依据。

## 2026-10-12 首轮取时与测量成本

本节保留工作区已有的第一轮数据。生产接入审查后补齐了浮点相减前检查与 RPC 非 Linux affinity 拒绝分支；最终二进制的复测见下方“生产接入审查后的复测”，两轮原始文件分别保留。

测量环境为 Linux 6.8.0-101、KVM x86-64、Intel Xeon Platinum 8255C，虚拟机提供 4 个逻辑 CPU，标准时钟的 clocksource 为 `kvm-clock`。GCC 14.2.0，Release `-O3 -DNDEBUG`，CMake `consumer-minimal` 加上前述 RPC、benchmark 和 epoll 选项。没有启用 LTO。每轮 2,000,000 次，共九轮；CPU 0/1 的测试先后运行，测量期间没有并行构建。

两个 CPU 的频率来源均为校准，约 2.49409 GHz，`has_invariant_frequency()` 为 false。原始数据保存启动报告、全部样本与极值：[CPU 0 输出](evidence/rdtsc_time_sources_2026-10-12_cpu0.txt)、[CPU 1 输出](evidence/rdtsc_time_sources_2026-10-12_cpu1.txt)。表格是包含循环和校验和开销的每操作纳秒中位数，未扣除空循环。

| 场景 | CPU 0 ns/op | CPU 1 ns/op |
|---|---:|---:|
| `steady_clock::now()` | 29.636 | 29.407 |
| `system_clock::now()` | 29.624 | 29.634 |
| `read_relaxed()` | 8.794 | 8.822 |
| intrinsic `__rdtsc()` 对照 | 8.793 | 8.839 |
| relaxed + Q32 纳秒换算 | 9.771 | 9.795 |
| relaxed + double 纳秒换算 | 10.592 | 10.626 |
| `read_start()` | 18.114 | 18.115 |
| `read_end()` | 18.569 | 18.603 |
| 有序单次读取 + Q32 换算 | 22.476 | 22.500 |
| 两次标准时钟读取的空区间 | 59.251 | 59.032 |
| 有序起止 + Q32 区间换算 | 40.072 | 40.198 |
| 空循环对照 | 0.326 | 0.327 |
| LRU get，不逐操作取时 | 16.001 | 16.719 |
| LRU get + 标准时钟逐操作计时 | 77.287 | 77.224 |
| LRU get + 有序 TSC 逐操作计时 | 69.597 | 69.787 |

在 CPU 0，裸读取比标准时钟减少约 70.3% 的读取耗时；加 Q32 纳秒换算减少约 67.0%。有序区间测量减少约 32.4%，LRU 逐操作计时的外层耗时减少约 10.0%。裸读取与 intrinsic 对照几乎相同，当前成本主要来自指令。读取函数已内联，不额外加入 `hot` 属性；热路径的 GCC 汇编核对确认读取只有计数器指令/屏障和寄存器组合，成功换算使用 `mulq` / `shrdq` 及范围检查，没有调用或除法。

完整约 7.6 秒测试中，计数器换算与 `steady_clock` 的差异分别为 +16.607 ppm 和 +21.417 ppm。这包含校准误差、`CLOCK_MONOTONIC_RAW` 与 `steady_clock` 的速率差异及读数顺序偏差，不能将其解释为已证实的纯校准误差或长期精度保证。虚拟机宿主机争用仍可能产生离群样本，原始极值保留在输出中。

### RPC 路由计时方式对比

在同一二进制、CPU 0 上交替运行 `steady` 和 `rdtsc` 九对样本，每次 2,000,000 次同步路由查找。原始结果见 [RPC 逐轮 CSV](evidence/rdtsc_rpc_route_2026-10-12.csv)，所有样本的错误数为零。

| 计时方式 | `lookup_per_s` 中位数 | 最小值 | 最大值 |
|---|---:|---:|---:|
| steady | 13,220,903.82 | 12,785,194.54 | 13,299,305.02 |
| rdtsc | 13,990,857.17 | 12,783,964.69 | 14,040,341.07 |

两个中位数之比提高约 5.82%，说明在这个负载上减少逐操作取时开销有收益。吞吐包含计时本身，路由生产逻辑没有修改；这组数据不代表网络 RPC 的生产吞吐提升，也不是外部产品对标。第一轮 rdtsc 和最后一轮 steady 等离群样本进一步说明，单轮结果不足以支持结论。

## 2026-10-12 生产接入审查后的复测

在完整构建和 CTest 结束后，以最终 Release 二进制顺序运行 RPC、utils CPU 0、utils CPU 1，期间没有并行构建或 CTest。环境仍为 Linux 6.8.0-101、KVM、Xeon Platinum 8255C、GCC 14.2.0、`-O3 -DNDEBUG`、无 LTO、`kvm-clock`。每个样本的启动报告确认 `affinity=pinned`。日志记录环境、二进制 SHA-256、计数器来源与全部原始样本；虚拟机宿主机负载仍未隔离。

RPC 路由 benchmark 每次 2,000,000 次查找，两种实现各九个样本，偶数轮按 steady/rdtsc、奇数轮按 rdtsc/steady 顺序执行。两种模式在同一个二进制中显式选择，没有跨进程或跨线程传递 tick。全部 18 个样本均为 `ok=2000000 errors=0`；前 10,000 次采集纳秒样本，外层吞吐使用 `steady_clock`。

| CPU 0 路由计时方式 | 样本数 | `lookup_per_s` 中位数 | 最小值 | 最大值 |
|---|---:|---:|---:|---:|
| steady | 9 | 13,272,162.20 | 13,145,354.93 | 13,325,999.24 |
| rdtsc | 9 | 13,985,678.23 | 13,752,120.14 | 14,050,144.45 |

两个吞吐中位数之比提高约 5.38%，仅表示本地路由微基准的逐操作测量开销减少。没有生产时间源被替换，不能解释为网络 RPC 业务吞吐提高。原始依据：[交替样本 CSV](evidence/rdtsc_rpc_route_review_2026-10-12.csv)、[环境与完整输出](evidence/rdtsc_rpc_route_review_2026-10-12.txt)。

utils 时间源与 LRU 场景分别在 CPU 0/1 固定单 CPU 运行；每个场景每轮 2,000,000 次，共九轮，奇偶轮反转场景顺序。下表为每操作纳秒中位数，包含循环、校验和与错误检查成本，不扣除空循环。

| 计时/采样场景 | CPU 0 ns/op | CPU 1 ns/op | 每 CPU 样本数 |
|---|---:|---:|---:|
| `steady_clock::now()` | 31.278 | 29.795 | 9 |
| `system_clock::now()` | 31.361 | 29.706 | 9 |
| `read_relaxed()` | 9.774 | 8.869 | 9 |
| relaxed + Q32 纳秒换算 | 11.042 | 9.870 | 9 |
| relaxed + double 纳秒换算（含相减前检查） | 11.802 | 10.914 | 9 |
| 两次 steady 读取的空区间 | 62.644 | 59.284 | 9 |
| 有序 RDTSC 起止 + Q32 区间换算 | 42.209 | 40.548 | 9 |
| LRU get，不逐操作取时 | 21.078 | 16.173 | 9 |
| LRU get + steady 逐操作计时 | 85.675 | 77.583 | 9 |
| LRU get + 有序 RDTSC 逐操作计时 | 74.052 | 69.953 | 9 |

CPU 0/1 校准频率分别为 2,494,088,001 / 2,494,099,348 Hz，架构 invariant 标记均为 false。完整约 8.18 / 7.63 秒区间相对 `steady_clock` 的差异为 +20.028 / +15.499 ppm；这些短期观测不证明长期或跨 CPU 同步。原始依据：[CPU 0 复测](evidence/rdtsc_time_sources_review_2026-10-12_cpu0.txt)、[CPU 1 复测](evidence/rdtsc_time_sources_review_2026-10-12_cpu1.txt)。

复测 RPC 时，从仓库根目录执行以下顺序；CPU 0 必须在允许集合内。每条命令都有独立初始化、权限检查和 affinity 回读，外层脚本不共享计数器。

```bash
for round in $(seq 0 8); do
  if [ $((round % 2)) -eq 0 ]; then
    clocks='steady rdtsc'
  else
    clocks='rdtsc steady'
  fi
  for clock in $clocks; do
    GALAY_BENCH_CPUS=0 build/rdtsc-eval/benchmark/cpp/rpc/benchmark_rpc_unary_mode_route_cache \
      -n 2000000 --clock "$clock" || exit 1
  done
done
```

## 验证与未覆盖范围

本轮在 `build/rdtsc-eval` 完成 `cmake --build build/rdtsc-eval -j 4`，全量 CTest 注册 292 项：289 项通过、3 项因 io_uring 条件跳过、0 项失败。首次全量运行的 `rpc.t21.rpc.client.concurrent.unary` 曾因 `Address already in use` 失败；该测试使用 `26000 + getpid() % 16000` 选端口，没有改动其代码，单独重跑及第二次完整 CTest 均通过。`utils.rdtsc` 另按指定正则单独执行并通过。最终全量摘要与检查命令见 [本轮验证记录](evidence/rdtsc_production_review_verification_2026-10-12.txt)。

本轮还实际验证了两个 benchmark 的正数迭代约束、RPC 未知/缺失参数拒绝、RDTSC 多 CPU 拒绝，以及 steady 在多 CPU 集合中的成功行为。`106_gen_module_prelude.py --module galay-utils --check` 和 `git diff --check` 通过；受影响文档的本地链接/锚点检查、盘点命令重放和本文完整 C++ 示例编译运行通过。生产行为没有变化，不新增没有消费者的生产测试或接口。

最终目录布局下，GCC Debug 的 28 个 utils CTest 全部通过，包含 `utils.string_time_random`、`utils.umbrella_surface` 和 `utils.rdtsc`。Release 的 `utils.rdtsc` 同样通过，测试文件显式保持断言启用。测试覆盖读取前进、短区间与标准时钟的差异、Q32 误差上界和 Linux 禁用 TSC 权限的失败返回；溢出分支依据实际频率及数值范围检查。

`mcpp run test_utils_import --locked --offline` 使用 LLVM 22.1.8 验证模块导出、`Rdtsc::create()` 链接与现有工具行为。迁移头文件的 include、CMake 总头示例和生成 module prelude 检查均通过。`config.install_include_layout` 验证安装前缀包含新路径、不存在 `core/`，并成功构建和运行包消费者。新增文档链接与源文件路径核验通过，本文 C++ 示例实际编译运行通过。仅头文件路径迁移没有改变原工具的 API 或计时语义，直接使用旧 `core/` include 的消费者必须更新路径。

当前没有 AArch64 运行环境，ARM 指令路径未经实机验证；也没有测试 macOS、其他 x86 CPU、非序列化 LFENCE 的旧 AMD 或虚拟机迁移。Bazel 清单已同步，但本机未安装 Bazel，不能宣称 Bazel 构建通过。性能结论只适用于上述机器和测量负载。

## 实现依据

- [Rdtsc 声明与热路径](../../../../src/cpp/galay-utils/system/rdtsc.hpp)：读取排序、Q32 换算、返回类型与前置条件。
- [Rdtsc 初始化](../../../../src/cpp/galay-utils/system/rdtsc.cc)：硬件检测、Linux 权限、频率来源与校准。
- [Rdtsc 测试](../../../../test/cpp/utils/t27_rdtsc.cc)：同 CPU 读数、精度边界与禁用 TSC 的失败路径。
- [取时 benchmark](../../../../benchmark/cpp/utils/b12_time_sources.cc) 与 [RPC 路由 benchmark](../../../../benchmark/cpp/rpc/b7_unary_mode_route_cache.cc)：样本生成和吞吐口径。
- [目录职责](01-架构设计.md)：原 `core/` 文件归入 `common/`、`algorithm/`、`system/`，不保留旧路径转发头。
