# Galay MPSC 通道使用指南

**版本:** v1.0  
**日期:** 2026-08-04  
**适用范围:** Bounded MPSC Channel (Latency-First & Throughput-First)

---

## 目录

1. [快速开始](#1-快速开始)
2. [策略选择](#2-策略选择)
3. [API 参考](#3-api-参考)
4. [性能调优](#4-性能调优)
5. [常见陷阱](#5-常见陷阱)
6. [故障排查](#6-故障排查)
7. [高级用法](#7-高级用法)

---

## 1. 快速开始

### 1.1 基本用法 - 延迟优先（默认）

```cpp
#include <galay-kernel/concurrency/mpsc/bounded_channel.h>

using galay::mpsc::BoundedChannel;

// 创建容量为 1024 的通道
BoundedChannel<int> channel(1024);

// 生产者：发送数据
bool sent = channel.trySend(42);
if (!sent) {
    // 通道满或已关闭
}

// 消费者：接收数据
auto result = channel.tryRecv();
if (result.has_value()) {
    int value = *result;
    // 处理 value
}

// 关闭通道
channel.close();
```

**适用场景**：
- 低延迟优先（P50 < 100ns）
- 1-4 个生产者
- 轻度到中度竞争

**性能特征**：
- 1P1C: ~153M msg/s
- 2P1C: ~101M msg/s
- 4P1C: ~171M msg/s

---

### 1.2 吞吐优先 - 高并发场景

```cpp
#include <galay-kernel/concurrency/mpsc/throughput_bounded_channel.h>

using galay::mpsc::ThroughputBoundedChannel;

// 创建通道：总容量 4096，预期最多 8 个生产者
ThroughputBoundedChannel<int> channel(
    4096,  // 总容量
    8      // 预期最大 producer 数
);

// API 与 BoundedChannel 一致
channel.trySend(42);
auto value = channel.tryRecv();
```

**适用场景**：
- 高吞吐优先（8P+ 场景）
- 4-32 个生产者
- 重度竞争

**性能特征**（预期）：
- 4P1C: ~220M msg/s (1.3x vs Latency-First)
- 8P1C: ~420M msg/s (1.5x vs Latency-First)
- 16P1C: ~650M msg/s (线性扩展)

---

## 2. 策略选择

### 2.1 决策树

```
你的场景是什么？
│
├─ 1-2 个生产者
│  └─→ 使用 BoundedChannel (延迟优先)
│
├─ 3-4 个生产者
│  ├─ 延迟敏感（P99 < 1μs）
│  │  └─→ BoundedChannel
│  └─ 吞吐优先
│     └─→ ThroughputBoundedChannel
│
└─ 8+ 个生产者
   └─→ ThroughputBoundedChannel（强烈推荐）
```

### 2.2 详细对比

| 维度             | BoundedChannel (延迟优先) | ThroughputBoundedChannel (吞吐优先) |
|------------------|---------------------------|-------------------------------------|
| **架构**         | 共享 ring + 指数退避       | Per-producer ring + ready-list      |
| **最佳场景**     | 1-4P，低延迟              | 8-32P，高吞吐                       |
| **延迟 P50**     | ~50ns                     | ~200ns                              |
| **延迟 P99**     | ~500ns                    | ~1μs                                |
| **吞吐 (4P)**    | 171M/s                    | ~220M/s                             |
| **吞吐 (8P)**    | ~280M/s                   | ~420M/s                             |
| **扩展性**       | 亚线性（8P+）             | 线性（16P+）                        |
| **内存占用**     | `capacity × sizeof(T)`    | `capacity × sizeof(T)` + 少量开销   |
| **代码复杂度**   | 低                        | 中                                  |

### 2.3 快速选择规则

**使用 BoundedChannel (延迟优先)** 当：
- ✅ 生产者数量 ≤ 4
- ✅ 延迟比吞吐更重要
- ✅ 内存受限
- ✅ 需要最简单的实现

**使用 ThroughputBoundedChannel (吞吐优先)** 当：
- ✅ 生产者数量 ≥ 8
- ✅ 需要线性扩展到 16P/32P
- ✅ 吞吐比延迟更重要
- ✅ 能容忍 ~4x 的延迟增加

---

## 3. API 参考

### 3.1 同步 API

#### 创建通道

```cpp
// 延迟优先
BoundedChannel<T> channel(size_t capacity);

// 吞吐优先
ThroughputBoundedChannel<T> channel(
    size_t totalCapacity,
    size_t maxProducers
);
```

**参数说明**：
- `capacity` / `totalCapacity`: 通道总容量（必须是 2 的幂次）
- `maxProducers`: 预期最大生产者数（仅 ThroughputBoundedChannel）

#### 发送（生产者侧）

```cpp
// 非阻塞发送
bool trySend(T&& value);

// 返回值：
//   true  - 发送成功
//   false - 通道满或已关闭
```

**示例**：
```cpp
if (channel.trySend(std::move(value))) {
    // 成功
} else {
    // 通道满，考虑：
    // 1. 增大容量
    // 2. 使用 sendAsync 挂起
    // 3. 丢弃数据（根据业务需求）
}
```

#### 接收（消费者侧）

```cpp
// 非阻塞接收
std::optional<T> tryRecv();

// 返回值：
//   std::optional<T> with value - 接收成功
//   std::nullopt               - 通道空或已关闭
```

**示例**：
```cpp
while (auto value = channel.tryRecv()) {
    process(*value);
}
// 通道空或已关闭
```

#### 批量接收

```cpp
// 批量接收（最多 maxCount 个）
template <typename OutputIt>
size_t tryRecvBatch(OutputIt out, size_t maxCount);

// 返回值：实际接收的数量
```

**示例**：
```cpp
std::vector<int> buffer;
buffer.reserve(64);

size_t count = channel.tryRecvBatch(
    std::back_inserter(buffer),
    64  // 最多 64 个
);

// 处理 buffer[0..count)
for (size_t i = 0; i < count; ++i) {
    process(buffer[i]);
}
```

#### 关闭通道

```cpp
void close();

// 效果：
//   - 后续 trySend 返回 false
//   - 已有数据仍可被 tryRecv 接收
//   - 数据耗尽后 tryRecv 返回 nullopt
```

---

### 3.2 异步 API (Awaitable)

#### 异步发送

```cpp
// 通道满时挂起协程，直到有空间或关闭
auto sendAsync(T&& value) -> BoundedSendAwaitable<T>;

// 使用方式：
auto result = co_await channel.sendAsync(std::move(value));
// result 类型：std::expected<void, IOError>
```

**示例**：
```cpp
Task<void> producer(BoundedChannel<int>& ch) {
    for (int i = 0; i < 1000; ++i) {
        auto result = co_await ch.sendAsync(i);
        if (!result) {
            // 通道已关闭
            co_return;
        }
    }
}
```

#### 异步接收

```cpp
// 通道空时挂起协程，直到有数据或关闭
auto recvAsync() -> BoundedRecvAwaitable<T>;

// 使用方式：
auto result = co_await channel.recvAsync();
// result 类型：std::expected<T, IOError>
```

**示例**：
```cpp
Task<void> consumer(BoundedChannel<int>& ch) {
    while (true) {
        auto result = co_await ch.recvAsync();
        if (!result) {
            // 通道已关闭且空
            co_return;
        }
        process(*result);
    }
}
```

#### 异步批量接收

```cpp
// 批量接收（至少 1 个，最多 maxCount 个）
template <typename OutputIt>
auto recvBatchAsync(OutputIt out, size_t maxCount)
    -> BoundedRecvBatchAwaitable<T>;

// 返回值：std::expected<size_t, IOError>
```

**示例**：
```cpp
Task<void> batchConsumer(BoundedChannel<int>& ch) {
    std::vector<int> buffer;
    buffer.reserve(64);
    
    while (true) {
        buffer.clear();
        auto result = co_await ch.recvBatchAsync(
            std::back_inserter(buffer),
            64
        );
        
        if (!result) {
            co_return;  // 通道关闭
        }
        
        size_t count = *result;
        // 处理 buffer[0..count)
        processBatch(buffer.data(), count);
    }
}
```

#### 超时支持

```cpp
// 发送超时
auto result = co_await channel.sendAsync(value)
    .withTimeout(std::chrono::milliseconds(100));
// result 类型：std::expected<void, IOError>
// 超时时 result.error() == kTimeout

// 接收超时
auto result = co_await channel.recvAsync()
    .withTimeout(std::chrono::milliseconds(100));
```

---

## 4. 性能调优

### 4.1 容量选择

#### 原则

**最小容量**（避免浪费内存）：
```
capacity ≥ producer_count × batch_size × 2
```

**推荐容量**（平衡性能和内存）：
```
capacity = 256     // 低延迟场景
capacity = 1024    // 中等吞吐
capacity = 4096    // 高吞吐场景
```

**注意**：容量必须是 2 的幂次（256, 512, 1024, 2048, 4096, ...）

#### 示例

```cpp
// 场景：4 个生产者，批量大小 16
// 最小容量：4 × 16 × 2 = 128
BoundedChannel<int> ch(256);  // 使用 256（2 倍最小值）

// 场景：8 个生产者，批量大小 64
// 最小容量：8 × 64 × 2 = 1024
ThroughputBoundedChannel<int> ch(4096, 8);  // 使用 4096
```

#### 容量过小的症状

- 频繁的 `trySend` 失败
- `sendAsync` 频繁挂起
- 吞吐量低于预期

**解决方案**：翻倍容量并重新测试

---

### 4.2 批量大小调优

#### 发送批量

对于 **ThroughputBoundedChannel**，批量发布大小在编译时固定：

```cpp
// 源码中（不可配置）
static constexpr size_t kPublishBatch = 16;

// 效果：每 16 条消息才更新一次原子 tail
// 减少 95% 的原子操作
```

**如需调整**：修改源码并重新编译（高级用户）

#### 接收批量

消费者应使用批量接收减少函数调用开销：

```cpp
// 不推荐：逐条接收
while (auto value = ch.tryRecv()) {
    process(*value);  // 每条消息一次函数调用
}

// 推荐：批量接收
std::vector<int> buffer;
buffer.reserve(64);

size_t count = ch.tryRecvBatch(std::back_inserter(buffer), 64);
for (size_t i = 0; i < count; ++i) {
    process(buffer[i]);  // 批量处理
}
```

**性能提升**：
- 批量 16: +30-40%
- 批量 64: +60-80%
- 批量 128: +80-100%

---

### 4.3 线程亲和性

#### NUMA 感知（Linux）

对于多 NUMA 节点系统：

```cpp
#include <sched.h>
#include <numa.h>

void pinToNumaNode(int node) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    
    // 获取节点上的 CPU
    for (int cpu = 0; cpu < numa_num_configured_cpus(); ++cpu) {
        if (numa_node_of_cpu(cpu) == node) {
            CPU_SET(cpu, &cpuset);
        }
    }
    
    pthread_setaffinity_np(pthread_self(), sizeof(cpuset), &cpuset);
}

// 生产者和消费者在同一 NUMA 节点
void producer() {
    pinToNumaNode(0);
    // ...
}

void consumer() {
    pinToNumaNode(0);  // 同节点
    // ...
}
```

**性能提升**：NUMA 亲和性可提升 20-40%

---

### 4.4 编译器优化

#### 推荐编译选项

```bash
# GCC/Clang
-O3 -march=native -DNDEBUG

# MSVC
/O2 /arch:AVX2 /DNDEBUG
```

#### CMake 配置

```cmake
set(CMAKE_BUILD_TYPE Release)
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")
```

**性能差异**：
- Debug (-O0): 基线
- Release (-O2): 2-3x
- Release (-O3 -march=native): 3-5x

---

## 5. 常见陷阱

### 5.1 容量不是 2 的幂次

**错误示例**：
```cpp
BoundedChannel<int> ch(1000);  // ❌ 1000 不是 2 的幂次
```

**症状**：编译错误或运行时断言失败

**解决方案**：
```cpp
BoundedChannel<int> ch(1024);  // ✅ 1024 = 2^10
```

---

### 5.2 生产者数量超出预期（ThroughputBoundedChannel）

**问题**：
```cpp
ThroughputBoundedChannel<int> ch(4096, 4);  // 预期 4 个生产者

// 但实际启动了 8 个生产者
for (int i = 0; i < 8; ++i) {
    std::thread([&ch] {
        ch.trySend(42);  // 前 4 个线程正常，后 4 个性能下降
    }).detach();
}
```

**症状**：
- 超出 `maxProducers` 的线程性能下降
- 共享 ring，退化为类似 BoundedChannel 的行为

**解决方案**：
```cpp
ThroughputBoundedChannel<int> ch(4096, 8);  // 增加 maxProducers
```

---

### 5.3 忘记关闭通道

**问题**：
```cpp
void test() {
    BoundedChannel<int> ch(256);
    
    std::thread producer([&ch] {
        // ...
    });
    
    std::thread consumer([&ch] {
        while (auto value = ch.tryRecv()) {  // 永远循环
            // ...
        }
    });
    
    producer.join();
    // ❌ 忘记 ch.close()
    consumer.join();  // 永远阻塞
}
```

**解决方案**：
```cpp
producer.join();
ch.close();  // ✅ 显式关闭
consumer.join();
```

---

### 5.4 在 trySend 失败后忙等待

**低效示例**：
```cpp
// ❌ 忙等待，浪费 CPU
while (!ch.trySend(value)) {
    // 空循环，100% CPU 占用
}
```

**推荐方案**：

**方案 1：使用异步 API**
```cpp
// ✅ 通道满时挂起协程，不占用 CPU
co_await ch.sendAsync(std::move(value));
```

**方案 2：有界重试 + 回退**
```cpp
// ✅ 重试几次后失败
for (int retry = 0; retry < 3; ++retry) {
    if (ch.trySend(value)) {
        break;
    }
    std::this_thread::sleep_for(std::chrono::microseconds(10));
}
```

---

### 5.5 在多个线程中消费（MPSC = Single Consumer！）

**错误示例**：
```cpp
BoundedChannel<int> ch(256);

// ❌ MPSC 通道只能有一个消费者
std::thread consumer1([&ch] { while (ch.tryRecv()) {} });
std::thread consumer2([&ch] { while (ch.tryRecv()) {} });
```

**症状**：未定义行为、数据竞争、崩溃

**解决方案**：
```cpp
// ✅ 使用 MPMC 通道
#include <galay-kernel/concurrency/mpmc/bounded_channel.h>
galay::mpmc::BoundedChannel<int> ch(256);
```

---

## 6. 故障排查

### 6.1 性能低于预期

#### 检查清单

1. **编译优化是否开启？**
   ```bash
   # 检查编译选项
   cmake -LA build | grep CMAKE_BUILD_TYPE
   # 应该是 Release，不是 Debug
   ```

2. **容量是否足够？**
   ```cpp
   // 容量太小会导致频繁满队列
   // 推荐：capacity ≥ producer_count × 128
   ```

3. **是否使用了正确的策略？**
   ```cpp
   // 8P+ 场景应使用 ThroughputBoundedChannel
   ```

4. **是否在 NUMA 系统上？**
   ```bash
   numactl --hardware  # 查看 NUMA 拓扑
   # 如果有多个节点，考虑线程亲和性
   ```

5. **是否有其他瓶颈？**
   ```cpp
   // 消费者处理逻辑太慢？
   // 网络 I/O？磁盘 I/O？
   ```

---

### 6.2 死锁或挂起

#### 症状

- `sendAsync` 或 `recvAsync` 永远不返回
- 线程 100% CPU 或完全不运行

#### 诊断

```bash
# 使用 gdb 查看调用栈
gdb -p <pid>
(gdb) thread apply all bt

# 或使用 perf
perf record -g -p <pid>
perf report
```

#### 常见原因

1. **忘记关闭通道**（见 5.3）
2. **容量为 0**（非法）
3. **协程调度器未运行**

---

### 6.3 崩溃或段错误

#### 症状

- Segmentation fault
- 内存访问违规

#### 诊断

```bash
# AddressSanitizer
cmake -DCMAKE_CXX_FLAGS="-fsanitize=address -g" -B build
./build/your_app

# ThreadSanitizer
cmake -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" -B build
./build/your_app
```

#### 常见原因

1. **多个消费者**（MPSC 违规，见 5.5）
2. **悬空引用**（通道销毁后仍访问）
3. **类型不满足 `BoundedValue` 约束**

---

## 7. 高级用法

### 7.1 零拷贝传递大对象

```cpp
struct LargeObject {
    std::array<uint8_t, 4096> data;
    // 移动构造成本低
    LargeObject(LargeObject&&) noexcept = default;
};

BoundedChannel<LargeObject> ch(256);

// 生产者：移动语义，无拷贝
LargeObject obj;
ch.trySend(std::move(obj));  // 零拷贝

// 消费者：移动语义，无拷贝
auto value = ch.tryRecv();  // 零拷贝
```

---

### 7.2 多阶段流水线

```cpp
// 三阶段流水线：Parser → Processor → Writer
BoundedChannel<RawData> stage1(1024);
BoundedChannel<ParsedData> stage2(1024);

// Parser
Task<void> parser() {
    while (auto raw = co_await stage1.recvAsync()) {
        ParsedData parsed = parse(*raw);
        co_await stage2.sendAsync(std::move(parsed));
    }
}

// Processor
Task<void> processor() {
    while (auto parsed = co_await stage2.recvAsync()) {
        // 处理
        write(*parsed);
    }
}
```

---

### 7.3 背压控制

```cpp
// 生产者根据通道状态调整速率
void adaptiveProducer(BoundedChannel<int>& ch) {
    int throttle = 0;
    
    for (int i = 0; i < 1000000; ++i) {
        if (!ch.trySend(i)) {
            // 通道满，增加节流
            throttle = std::min(throttle + 1, 10);
            std::this_thread::sleep_for(
                std::chrono::microseconds(throttle * 10)
            );
        } else {
            // 发送成功，减少节流
            throttle = std::max(throttle - 1, 0);
        }
    }
}
```

---

### 7.4 性能监控

```cpp
// 自定义包装类，统计指标
template <typename T>
class MonitoredChannel {
public:
    explicit MonitoredChannel(size_t capacity)
        : m_channel(capacity) {}
    
    bool trySend(T&& value) {
        auto start = std::chrono::high_resolution_clock::now();
        bool success = m_channel.trySend(std::move(value));
        auto duration = std::chrono::high_resolution_clock::now() - start;
        
        m_sendCount.fetch_add(1, std::memory_order_relaxed);
        if (!success) {
            m_sendFailures.fetch_add(1, std::memory_order_relaxed);
        }
        m_sendLatency.fetch_add(
            std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count(),
            std::memory_order_relaxed
        );
        
        return success;
    }
    
    void printStats() {
        uint64_t total = m_sendCount.load();
        uint64_t failures = m_sendFailures.load();
        uint64_t latency = m_sendLatency.load();
        
        std::cout << "Sends: " << total << "\n"
                  << "Failures: " << failures 
                  << " (" << (100.0 * failures / total) << "%)\n"
                  << "Avg Latency: " << (latency / total) << " ns\n";
    }
    
private:
    BoundedChannel<T> m_channel;
    std::atomic<uint64_t> m_sendCount{0};
    std::atomic<uint64_t> m_sendFailures{0};
    std::atomic<uint64_t> m_sendLatency{0};
};
```

---

## 总结

**关键要点**：

1. **策略选择**：1-4P 用 `BoundedChannel`，8P+ 用 `ThroughputBoundedChannel`
2. **容量设置**：至少 `producer_count × batch_size × 2`，推荐 1024-4096
3. **批量接收**：使用 `tryRecvBatch` 可提升 60-80% 吞吐
4. **异步优先**：用 `sendAsync`/`recvAsync` 避免忙等待
5. **显式关闭**：始终调用 `close()` 通知消费者
6. **单消费者**：MPSC = Single Consumer，多消费者用 MPMC

**下一步**：
- 阅读 [最佳实践](14_best_practices.md)
- 查看 [性能分析](01_performance_analysis.md)
- 运行基准测试验证你的场景
