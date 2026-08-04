# Galay MPSC 通道最佳实践

**版本:** v1.0  
**日期:** 2026-08-04  
**目标读者:** 使用 Galay MPSC 通道的开发者

---

## 目录

1. [容量选择指南](#1-容量选择指南)
2. [Producer 数量规划](#2-producer-数量规划)
3. [批量大小调优](#3-批量大小调优)
4. [内存使用优化](#4-内存使用优化)
5. [监控和指标](#5-监控和指标)
6. [性能基准测试方法](#6-性能基准测试方法)
7. [生产环境检查清单](#7-生产环境检查清单)

---

## 1. 容量选择指南

### 1.1 容量计算公式

#### 基本公式

```
最小容量 = producer_count × avg_burst_size × 2
推荐容量 = 向上取整到最近的 2 的幂次
```

**原理**：
- `producer_count × avg_burst_size`: 所有生产者同时发送的最大消息数
- `× 2`: 安全系数，避免频繁满队列
- 2 的幂次：环形缓冲区性能优化要求（位掩码代替取模）

#### 示例计算

**场景 1：低延迟日志系统**
```
producer_count = 4 (4 个工作线程)
avg_burst_size = 10 (每次突发 10 条日志)
最小容量 = 4 × 10 × 2 = 80
推荐容量 = 128 (2^7，向上取整)
```

**场景 2：高吞吐数据管道**
```
producer_count = 8 (8 个数据采集线程)
avg_burst_size = 64 (批量发送 64 条)
最小容量 = 8 × 64 × 2 = 1024
推荐容量 = 2048 或 4096 (留更多余量)
```

**场景 3：实时事件处理**
```
producer_count = 16 (16 个事件源)
avg_burst_size = 32 (每批 32 个事件)
最小容量 = 16 × 32 × 2 = 1024
推荐容量 = 2048 或 4096
```

---

### 1.2 容量调优决策树

```
开始
│
├─ 观察到频繁的 trySend 失败（>5%）？
│  └─ 是 → 容量翻倍（256→512→1024→2048）
│
├─ 内存占用过高？
│  └─ 是 → 减少容量，但不低于最小值
│
└─ 性能满足需求且内存合理？
   └─ 是 → 当前容量合适，无需调整
```

---

### 1.3 不同场景的推荐容量

| 场景类型           | Producer 数 | 推荐容量 | 原因                              |
|--------------------|-------------|----------|-----------------------------------|
| 低延迟服务         | 1-4         | 256      | 小容量 = 低延迟，cache-friendly   |
| 中等吞吐服务       | 4-8         | 1024     | 平衡延迟和吞吐                    |
| 高吞吐数据管道     | 8-16        | 4096     | 大容量避免背压，提升吞吐          |
| 批处理系统         | 8-32        | 8192     | 极大容量支持批量操作              |

---

### 1.4 代码示例

```cpp
// 低延迟场景
BoundedChannel<LogEntry> logChannel(256);  // 小容量

// 高吞吐场景
ThroughputBoundedChannel<DataPacket> dataChannel(
    4096,  // 大容量
    8      // 8 个生产者
);

// 动态调整（高级）
size_t calculateCapacity(size_t producerCount, size_t avgBurst) {
    size_t minCap = producerCount * avgBurst * 2;
    // 向上取整到 2 的幂次
    size_t capacity = 1;
    while (capacity < minCap) {
        capacity <<= 1;
    }
    return capacity;
}

auto cap = calculateCapacity(8, 64);  // 返回 1024
BoundedChannel<int> ch(cap);
```

**预期效果**：
- 容量合适：trySend 失败率 < 5%
- 容量过小：trySend 失败率 > 20%
- 容量过大：内存浪费，缓存不友好

---

## 2. Producer 数量规划

### 2.1 Producer 数量与性能的关系

#### 性能曲线

```
性能 (msg/s)
    │
    │ Throughput-First (线性扩展)
400M│              ╱
    │            ╱
300M│          ╱
    │        ╱  Latency-First (亚线性)
200M│      ╱   ╱
    │    ╱   ╱
100M│  ╱   ╱
    │╱___╱
    └────────────────────→ Producer 数量
    1  2  4  8  16  32
```

**关键拐点**：
- **1-4P**: BoundedChannel 和 ThroughputBoundedChannel 性能相近
- **4-8P**: ThroughputBoundedChannel 开始显著领先
- **8P+**: ThroughputBoundedChannel 保持线性扩展，BoundedChannel 增速放缓

---

### 2.2 如何确定 Producer 数量

#### 方法 1：基于硬件线程数

```cpp
#include <thread>

// 规则：Producer 数 ≤ 物理核心数
size_t producerCount = std::thread::hardware_concurrency() / 2;

// 示例：8 核 CPU
// producerCount = 4（预留一半给消费者和系统）
```

**原理**：避免过度订阅导致上下文切换开销。

---

#### 方法 2：基于工作负载

```cpp
// 计算公式
producer_count = total_workload / single_producer_capacity

// 示例：需要处理 500M msg/s
// 单个 producer 实测可达 150M msg/s
producer_count = 500 / 150 ≈ 4（向上取整）
```

---

#### 方法 3：压力测试确定

```cpp
// 逐步增加 producer 数量，观察吞吐量
for (size_t p = 1; p <= 32; p *= 2) {
    auto throughput = benchmark(p);
    std::cout << p << "P: " << throughput << " msg/s\n";
    
    // 当吞吐量不再线性增长时停止
    if (p > 1 && throughput < lastThroughput * 1.5) {
        optimalProducers = p / 2;
        break;
    }
    lastThroughput = throughput;
}
```

**预期效果**：
- 找到吞吐量增长放缓的拐点
- 选择该拐点前的 producer 数量

---

### 2.3 ThroughputBoundedChannel 的 maxProducers 设置

#### 原则

```cpp
maxProducers ≥ 实际 producer 线程数
```

**原因**：每个 producer 需要独占一个 ring，超出 `maxProducers` 的线程会共享 ring，性能下降。

---

#### 代码示例

```cpp
// 错误：maxProducers 设置过小
ThroughputBoundedChannel<int> ch(4096, 4);  // 只支持 4 个
for (int i = 0; i < 8; ++i) {  // 但启动了 8 个
    std::thread([&ch] {
        // 后 4 个线程性能下降
    }).detach();
}

// 正确：maxProducers 匹配实际数量
ThroughputBoundedChannel<int> ch(4096, 8);  // 支持 8 个
for (int i = 0; i < 8; ++i) {
    std::thread([&ch] {
        // 所有线程都有独占 ring
    }).detach();
}

// 推荐：留 20% 余量
size_t expectedProducers = 8;
size_t maxProducers = expectedProducers * 1.2;  // 9.6 → 10
ThroughputBoundedChannel<int> ch(4096, maxProducers);
```

**预期效果**：
- 正确设置：所有 producer 线性扩展
- 设置过小：超出部分性能下降 50%+

---

## 3. 批量大小调优

### 3.1 批量接收的重要性

#### 性能对比

| 接收方式       | 吞吐量   | 相对性能 |
|----------------|----------|----------|
| 逐条接收       | 100M/s   | 1.0x     |
| 批量 16        | 140M/s   | 1.4x     |
| 批量 64        | 180M/s   | 1.8x     |
| 批量 128       | 190M/s   | 1.9x     |

**原理**：
- 减少函数调用开销
- 提升缓存局部性
- 减少分支预测失败

---

### 3.2 批量大小选择

#### 推荐值

```cpp
// 低延迟场景
constexpr size_t BATCH_SIZE = 16;

// 中等吞吐场景
constexpr size_t BATCH_SIZE = 64;

// 高吞吐场景
constexpr size_t BATCH_SIZE = 128;

// 极限吞吐场景
constexpr size_t BATCH_SIZE = 256;
```

#### 权衡

```
批量大小
    │
    │  ┌── 吞吐量（收益递减）
    │  │
    │  │ ┌─ 延迟（线性增长）
    │ ╱│╱
    │╱ │
    └───┴──────────→ 批量大小
    16 64 128 256
```

**建议**：
- 延迟敏感：16-32
- 平衡场景：64
- 吞吐优先：128-256

---

### 3.3 代码示例

#### 基本批量接收

```cpp
void consumer(BoundedChannel<int>& ch) {
    constexpr size_t BATCH = 64;
    std::vector<int> buffer;
    buffer.reserve(BATCH);
    
    while (true) {
        buffer.clear();
        size_t count = ch.tryRecvBatch(
            std::back_inserter(buffer),
            BATCH
        );
        
        if (count == 0) {
            break;  // 通道关闭且空
        }
        
        // 批量处理
        processBatch(buffer.data(), count);
    }
}
```

**预期效果**：吞吐量提升 60-80% vs 逐条接收

---

#### 自适应批量大小

```cpp
class AdaptiveBatchConsumer {
public:
    void consume(BoundedChannel<int>& ch) {
        while (true) {
            m_buffer.clear();
            size_t count = ch.tryRecvBatch(
                std::back_inserter(m_buffer),
                m_currentBatch
            );
            
            if (count == 0) break;
            
            processBatch(m_buffer.data(), count);
            
            // 根据实际接收量调整批量大小
            adjustBatchSize(count);
        }
    }
    
private:
    void adjustBatchSize(size_t actualCount) {
        if (actualCount >= m_currentBatch * 0.9) {
            // 接近上限，增大批量
            m_currentBatch = std::min(m_currentBatch * 2, MAX_BATCH);
        } else if (actualCount < m_currentBatch * 0.3) {
            // 远低于上限，减小批量（降低延迟）
            m_currentBatch = std::max(m_currentBatch / 2, MIN_BATCH);
        }
    }
    
    static constexpr size_t MIN_BATCH = 16;
    static constexpr size_t MAX_BATCH = 256;
    
    std::vector<int> m_buffer;
    size_t m_currentBatch = 64;
};
```

**预期效果**：
- 低负载时自动减小批量（低延迟）
- 高负载时自动增大批量（高吞吐）

---

## 4. 内存使用优化

### 4.1 内存占用计算

#### 公式

```
总内存 = capacity × sizeof(T) + overhead
```

**Overhead 组成**：
- `BoundedChannel`: ~128 字节（控制结构）
- `ThroughputBoundedChannel`: ~128 + maxProducers × 256 字节

---

#### 示例计算

```cpp
// BoundedChannel
struct Message {
    uint64_t id;
    uint64_t timestamp;
    std::array<uint8_t, 64> payload;
};  // sizeof(Message) = 80 字节

BoundedChannel<Message> ch(1024);
// 内存 = 1024 × 80 + 128 ≈ 82KB

// ThroughputBoundedChannel
ThroughputBoundedChannel<Message> ch(4096, 8);
// 内存 = 4096 × 80 + 128 + 8 × 256 ≈ 330KB
```

---

### 4.2 减少内存占用的技巧

#### 技巧 1：使用指针或索引

```cpp
// 方案 A：直接存储大对象（内存占用大）
struct LargeMessage {
    std::array<uint8_t, 4096> data;
};
BoundedChannel<LargeMessage> ch(1024);
// 内存 = 1024 × 4096 = 4MB

// 方案 B：存储智能指针（内存占用小）
BoundedChannel<std::unique_ptr<LargeMessage>> ch(1024);
// 内存 = 1024 × 8 = 8KB（通道本身）
// 实际对象在堆上按需分配
```

**预期效果**：内存占用减少 500x

---

#### 技巧 2：动态调整容量

```cpp
class DynamicChannel {
public:
    DynamicChannel(size_t initialCap)
        : m_channel(initialCap)
        , m_capacity(initialCap) {}
    
    void scaleUp() {
        if (m_capacity >= 8192) return;
        
        // 创建更大的通道
        auto newCh = BoundedChannel<int>(m_capacity * 2);
        
        // 迁移数据
        while (auto value = m_channel.tryRecv()) {
            newCh.trySend(*value);
        }
        
        m_channel = std::move(newCh);
        m_capacity *= 2;
    }
    
    void scaleDown() {
        if (m_capacity <= 256) return;
        // 类似实现
    }
    
private:
    BoundedChannel<int> m_channel;
    size_t m_capacity;
};
```

**注意**：迁移期间有短暂的性能影响。

---

### 4.3 内存池优化（高级）

```cpp
template <typename T>
class PooledChannel {
public:
    PooledChannel(size_t capacity, size_t poolSize)
        : m_channel(capacity)
        , m_pool(poolSize) {
        // 预分配对象池
        for (size_t i = 0; i < poolSize; ++i) {
            m_pool.emplace_back(std::make_unique<T>());
        }
    }
    
    std::unique_ptr<T> acquire() {
        if (m_pool.empty()) {
            return std::make_unique<T>();
        }
        auto obj = std::move(m_pool.back());
        m_pool.pop_back();
        return obj;
    }
    
    void release(std::unique_ptr<T> obj) {
        if (m_pool.size() < m_maxPoolSize) {
            m_pool.push_back(std::move(obj));
        }
    }
    
private:
    BoundedChannel<std::unique_ptr<T>> m_channel;
    std::vector<std::unique_ptr<T>> m_pool;
    size_t m_maxPoolSize;
};
```

**预期效果**：
- 减少动态分配次数 90%+
- 降低内存碎片

---

## 5. 监控和指标

### 5.1 关键性能指标（KPI）

| 指标                | 单位      | 目标值            | 含义                      |
|---------------------|-----------|-------------------|---------------------------|
| 吞吐量              | msg/s     | ≥ 业务需求        | 每秒处理的消息数          |
| 发送延迟 P50        | ns        | < 100             | 50% 发送操作的延迟        |
| 发送延迟 P99        | ns        | < 1000            | 99% 发送操作的延迟        |
| 接收延迟 P50        | ns        | < 100             | 50% 接收操作的延迟        |
| trySend 失败率      | %         | < 5%              | 发送失败的百分比          |
| 通道利用率          | %         | 30-70%            | 平均占用率                |

---

### 5.2 监控实现

#### 方案 1：包装类

```cpp
template <typename T>
class MonitoredChannel {
public:
    explicit MonitoredChannel(size_t capacity)
        : m_channel(capacity) {}
    
    bool trySend(T&& value) {
        auto start = now();
        bool success = m_channel.trySend(std::move(value));
        auto duration = now() - start;
        
        recordSend(success, duration);
        return success;
    }
    
    std::optional<T> tryRecv() {
        auto start = now();
        auto result = m_channel.tryRecv();
        auto duration = now() - start;
        
        recordRecv(result.has_value(), duration);
        return result;
    }
    
    struct Stats {
        uint64_t sendCount;
        uint64_t sendFailures;
        uint64_t recvCount;
        uint64_t recvEmpty;
        uint64_t sendLatencyP50;
        uint64_t sendLatencyP99;
    };
    
    Stats getStats() const {
        // 计算统计信息
        return /* ... */;
    }
    
private:
    BoundedChannel<T> m_channel;
    // 统计数据结构
    // ...
};
```

---

#### 方案 2：Prometheus 集成

```cpp
#include <prometheus/counter.h>
#include <prometheus/histogram.h>

class MetricsChannel {
public:
    MetricsChannel(/* ... */)
        : m_sendCounter(prometheus::BuildCounter()
            .Name("channel_send_total")
            .Register(*registry))
        , m_sendLatency(prometheus::BuildHistogram()
            .Name("channel_send_duration_ns")
            .Register(*registry)) {}
    
    bool trySend(T&& value) {
        auto start = now();
        bool success = m_channel.trySend(std::move(value));
        auto duration = now() - start;
        
        m_sendCounter.Add({"status", success ? "success" : "failed"}).Increment();
        m_sendLatency.Observe(duration);
        
        return success;
    }
};
```

---

### 5.3 告警规则

```yaml
# Prometheus 告警配置
groups:
  - name: channel_alerts
    rules:
      # 发送失败率过高
      - alert: HighSendFailureRate
        expr: rate(channel_send_total{status="failed"}[5m]) / rate(channel_send_total[5m]) > 0.05
        annotations:
          summary: "通道发送失败率超过 5%"
          
      # 延迟过高
      - alert: HighLatency
        expr: histogram_quantile(0.99, channel_send_duration_ns) > 1000
        annotations:
          summary: "P99 延迟超过 1μs"
          
      # 吞吐量下降
      - alert: ThroughputDrop
        expr: rate(channel_send_total[5m]) < 100000000
        annotations:
          summary: "吞吐量低于 100M msg/s"
```

---

## 6. 性能基准测试方法

### 6.1 基准测试框架

```cpp
#include <benchmark/benchmark.h>

static void BM_BoundedChannelThroughput(benchmark::State& state) {
    const size_t capacity = state.range(0);
    const size_t producerCount = state.range(1);
    
    BoundedChannel<int> ch(capacity);
    
    // 启动生产者线程
    std::vector<std::thread> producers;
    for (size_t i = 0; i < producerCount; ++i) {
        producers.emplace_back([&ch] {
            for (int j = 0; j < 1000000; ++j) {
                while (!ch.trySend(j)) {}
            }
        });
    }
    
    // 消费者主循环
    uint64_t recvCount = 0;
    for (auto _ : state) {
        if (auto value = ch.tryRecv()) {
            ++recvCount;
        }
    }
    
    ch.close();
    for (auto& t : producers) t.join();
    
    state.SetItemsProcessed(recvCount);
    state.SetBytesProcessed(recvCount * sizeof(int));
}

BENCHMARK(BM_BoundedChannelThroughput)
    ->Args({256, 1})
    ->Args({256, 2})
    ->Args({256, 4})
    ->Args({1024, 4})
    ->Args({4096, 8});
    
BENCHMARK_MAIN();
```

---

### 6.2 测试矩阵

| 维度             | 测试点                     |
|------------------|----------------------------|
| Producer 数量    | 1, 2, 4, 8, 16, 32         |
| 容量             | 64, 256, 1024, 4096, 8192  |
| 消息大小         | 8B, 64B, 256B, 1KB         |
| 批量大小         | 1, 16, 64, 128, 256        |

---

### 6.3 对比测试

```bash
# 运行基准测试
cd build
./benchmark_kernel_bounded_channel_throughput

# 对比 Rust Crossbeam
cd benchmark/cpp/kernel/compare
./run_crossbeam_comparison.sh

# 生成报告
python3 generate_report.py \
    --galay results/galay.json \
    --crossbeam results/crossbeam.json \
    --output report.md
```

---

## 7. 生产环境检查清单

### 7.1 部署前检查

- [ ] **容量设置合理**（≥ producer_count × avg_burst × 2）
- [ ] **使用正确的策略**（1-4P 用 BoundedChannel，8P+ 用 ThroughputBoundedChannel）
- [ ] **编译优化开启**（Release 模式，-O3 -march=native）
- [ ] **监控已配置**（Prometheus / 自定义）
- [ ] **告警规则已设置**（失败率、延迟、吞吐量）
- [ ] **压力测试通过**（3x 峰值负载）
- [ ] **内存占用在预算内**（capacity × sizeof(T) < 限制）
- [ ] **错误处理完善**（trySend 失败、通道关闭）

---

### 7.2 运行时监控

```cpp
// 定期打印统计信息
void monitoringLoop(MonitoredChannel<int>& ch) {
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        
        auto stats = ch.getStats();
        
        // 检查关键指标
        double failureRate = 
            double(stats.sendFailures) / stats.sendCount;
        
        if (failureRate > 0.05) {
            LOG_WARN("High send failure rate: {}%", failureRate * 100);
            // 触发告警或自动扩容
        }
        
        LOG_INFO("Throughput: {} msg/s, P99 Latency: {} ns",
            stats.sendCount / 10,
            stats.sendLatencyP99);
    }
}
```

---

### 7.3 故障恢复

#### 通道满处理

```cpp
bool sendWithRetry(BoundedChannel<int>& ch, int value, int maxRetries = 3) {
    for (int i = 0; i < maxRetries; ++i) {
        if (ch.trySend(value)) {
            return true;
        }
        
        // 指数退避
        std::this_thread::sleep_for(
            std::chrono::microseconds(10 << i)
        );
    }
    
    // 最终失败：记录日志或丢弃
    LOG_ERROR("Failed to send after {} retries", maxRetries);
    return false;
}
```

---

#### 通道关闭恢复

```cpp
void resilientConsumer(BoundedChannel<int>& ch) {
    while (true) {
        auto result = ch.tryRecv();
        
        if (!result) {
            if (ch.isClosed()) {
                LOG_INFO("Channel closed, exiting");
                break;
            }
            // 通道空但未关闭，继续等待
            std::this_thread::yield();
            continue;
        }
        
        process(*result);
    }
}
```

---

## 总结

### 核心要点

1. **容量选择**: `producer_count × avg_burst × 2`，向上取整到 2 的幂次
2. **策略选择**: 1-4P 用 BoundedChannel，8P+ 用 ThroughputBoundedChannel
3. **批量接收**: 使用 tryRecvBatch，批量 64 可提升 60-80% 吞吐
4. **内存优化**: 对大对象使用指针，避免直接存储
5. **监控指标**: 吞吐量、延迟、失败率、利用率
6. **基准测试**: 多维度测试（producer 数、容量、消息大小、批量）

### 性能目标

| 场景  | 策略             | 预期吞吐量 | 预期延迟 P99 |
|-------|------------------|------------|--------------|
| 1P1C  | BoundedChannel   | 153M/s     | 100ns        |
| 4P1C  | BoundedChannel   | 171M/s     | 500ns        |
| 8P1C  | ThroughputFirst  | 420M/s     | 1μs          |
| 16P1C | ThroughputFirst  | 650M/s     | 2μs          |

### 下一步

- 运行 [基准测试](../benchmark/cpp/kernel/README.md)
- 阅读 [使用指南](13_usage_guide.md)
- 查看 [性能分析](01_performance_analysis.md)
