# 吞吐优先策略（Throughput-First）实施文档

## 1. 实施概述

### 1.1 目标

实现 per-producer ring 架构，追求最高性能的 bounded MPSC 通道：
- **4P1C**: ≥200M/s（vs 当前 171.5M/s）
- **8P1C**: ≥400M/s（线性扩展）
- **16P1C**: ≥600M/s（持续扩展）

### 1.2 核心设计

**Per-Producer Ring 架构**：
```
Producer 1 → RingBuffer 1 ┐
Producer 2 → RingBuffer 2 ├─→ Ready List → Consumer (轮询)
Producer 3 → RingBuffer 3 │
Producer N → RingBuffer N ┘
```

**关键特性**：
1. 每个 producer 独占一个 ring（无共享 tail CAS）
2. Ready-list 管理有数据的 ring
3. Consumer 按配额轮询（避免饥饿）
4. 批量发布 + 批量消费

---

## 2. 架构设计

### 2.1 数据结构

#### ProducerRing（单个 producer 的 ring）

```cpp
struct ProducerRing {
    // Producer 写（缓存行对齐避免 false sharing）
    alignas(64) size_t localTail = 0;           // 本地 cursor（无原子操作）
    alignas(64) std::atomic<size_t> tail{0};    // 发布的 tail
    
    // Consumer 写
    alignas(64) std::atomic<size_t> head{0};
    
    // Ready-list 管理
    alignas(64) std::atomic<bool> active{false};
    ProducerRing* readyNext = nullptr;
    
    // Ring 配置
    size_t capacity;
    size_t mask;
    std::vector<Slot> slots;
    
    // 批量控制
    size_t batchCount = 0;
    static constexpr size_t kPublishBatch = 16;
};
```

**设计要点**：
- `localTail`：producer 本地 cursor，热路径无原子操作
- `tail`：批量发布时才更新（减少原子写频率）
- 缓存行对齐：避免 producer-consumer false sharing

#### ThroughputBoundedChannel（通道主体）

```cpp
class ThroughputBoundedChannel {
private:
    // Registry（所有 ring 的链表）
    alignas(64) std::atomic<ProducerRing*> m_ringHead{nullptr};
    
    // Ready stack（有数据的 ring）
    alignas(64) std::atomic<ProducerRing*> m_readyStack{nullptr};
    
    // Consumer 侧 ready list
    ProducerRing* m_readyHead = nullptr;
    ProducerRing* m_readyTail = nullptr;
    size_t m_activeRingCount = 0;
    size_t m_quotaUsed = 0;
    
    // 配置
    const size_t m_totalCapacity;
    const size_t m_maxProducers;
    const size_t m_ringCapacity;  // = totalCapacity / maxProducers
    
    static constexpr size_t kConsumerQuota = 64;
};
```

---

### 2.2 核心算法

#### 2.2.1 Producer 发送流程

```cpp
bool trySend(T&& value) {
    ProducerRing* ring = getProducerRing();  // 线程本地缓存
    
    // 1. 无 CAS 认领 slot（producer 独占）
    size_t position = ring->localTail++;
    size_t index = position & ring->mask;
    Slot& slot = ring->slots[index];
    
    // 2. 等待 slot 可用（consumer 释放）
    while (slot.sequence.load(acquire) != position) {
        cpuPause();  // 轻量级退避
    }
    
    // 3. 写入值
    construct_at(slot.value(), std::move(value));
    slot.sequence.store(position + 1, release);
    
    // 4. 批量发布（减少原子操作）
    if (++ring->batchCount >= kPublishBatch) {
        ring->tail.store(ring->localTail, release);
        ring->batchCount = 0;
        activateRing(ring);  // 加入 ready list
    }
    
    return true;
}
```

**优势**：
- ✅ 无 CAS 竞争：`localTail++` 是本地变量
- ✅ 批量发布：16 条消息才更新一次原子 `tail`
- ✅ 轻量级等待：只等待 consumer 释放 slot

#### 2.2.2 Consumer 接收流程

```cpp
std::optional<T> tryRecv() {
    // 1. 刷新 ready list（从 stack 摘取新 ring）
    if (m_readyHead == nullptr || m_quotaUsed == 0) {
        appendReadyRings();
    }
    
    // 2. 轮询 active rings（配额限制）
    size_t remaining = m_activeRingCount;
    while (m_readyHead != nullptr && remaining > 0) {
        ProducerRing* ring = m_readyHead;
        
        // a. 尝试消费
        auto value = tryPopRing(ring);
        if (value.has_value()) {
            ++m_quotaUsed;
            if (m_quotaUsed >= kConsumerQuota) {
                rotateReadyHead();  // 公平性
            }
            return value;
        }
        
        // b. 当前 ring 空，轮转
        rotateReadyHead();
        --remaining;
    }
    
    return std::nullopt;
}
```

**公平性保证**：
- 每个 ring 最多连续消费 64 条（`kConsumerQuota`）
- 然后轮转到下一个 ring
- 避免活跃 producer 饿死稀疏 producer

#### 2.2.3 Ready-List 管理

**激活流程**（producer 侧）：

```cpp
void activateRing(ProducerRing& ring) {
    if (ring.active.exchange(true, acq_rel)) {
        return;  // 已激活
    }
    
    // 加入 ready stack（lock-free）
    ProducerRing* ready = m_readyStack.load(relaxed);
    do {
        ring.readyNext = ready;
    } while (!m_readyStack.compare_exchange_weak(
        ready, &ring, seq_cst, relaxed));
}
```

**摘取流程**（consumer 侧）：

```cpp
void appendReadyRings() {
    // 原子交换整个 stack
    ProducerRing* stack = m_readyStack.exchange(nullptr, seq_cst);
    
    // 反转 stack 为 list
    ProducerRing* ready = nullptr;
    while (stack != nullptr) {
        ProducerRing* next = stack->readyNext;
        stack->readyNext = ready;
        ready = stack;
        stack = next;
    }
    
    // 追加到 ready list 尾部
    if (m_readyTail == nullptr) {
        m_readyHead = ready;
    } else {
        m_readyTail->readyNext = ready;
    }
    m_readyTail = findTail(ready);
}
```

---

### 2.3 与 Unbounded 的对比

| 特性            | Unbounded                  | Throughput Bounded           |
|-----------------|----------------------------|------------------------------|
| Producer Ring   | 分块链表（4KB block）      | 固定容量 ring                |
| Slot 分配       | 动态分配 block             | 预分配，循环复用             |
| Backpressure    | 无（无界）                 | 有（等待 slot 释放）         |
| 内存占用        | 动态增长                   | 固定（totalCapacity）        |
| 批量发布        | ✅                          | ✅                            |
| Ready-list      | ✅                          | ✅                            |
| 配额轮询        | ✅                          | ✅                            |

**核心相似性**：
- 两者都使用 per-producer 架构
- 都有 ready-list 管理
- 都支持批量优化

**关键差异**：
- Bounded 使用固定 ring（内存可控）
- Bounded 有 backpressure（等待 slot）

---

## 3. 性能优化

### 3.1 缓存行对齐

**False Sharing 消除**：

```cpp
struct ProducerRing {
    alignas(64) size_t localTail;       // Producer 热路径
    alignas(64) std::atomic<size_t> tail;
    alignas(64) std::atomic<size_t> head; // Consumer 热路径
    alignas(64) std::atomic<bool> active;
    // ...
};
```

**效果**：
- Producer 写 `localTail`/`tail`，不会干扰 consumer 读 `head`
- Consumer 写 `head`，不会干扰 producer 读 `tail`

### 3.2 批量优化

**发布批量**（Producer）：

```cpp
static constexpr size_t kPublishBatch = 16;

// 累积 16 条消息后才发布
if (++ring->batchCount >= kPublishBatch) {
    ring->tail.store(ring->localTail, release);
    ring->batchCount = 0;
}
```

**效果**：
- 原子操作频率：每 16 条 → 1 次
- 减少 95% 的原子写

**消费批量**（Consumer）：

```cpp
static constexpr size_t kConsumerQuota = 64;

// 每个 ring 最多连续消费 64 条
for (size_t i = 0; i < kConsumerQuota; ++i) {
    auto value = tryPopRing(ring);
    if (!value.has_value()) break;
    process(value);
}
```

**效果**：
- 减少 ring 切换频率
- 提升缓存局部性

### 3.3 线程本地缓存

**Producer Handle 缓存**：

```cpp
static thread_local ProducerHandle g_handle;

ProducerHandle* getProducerHandle() {
    if (g_handle.validFor(this, m_generation)) {
        return &g_handle;  // 快速路径
    }
    
    // 慢路径：获取/创建 ring
    g_handle = acquireProducerRing();
    return &g_handle;
}
```

**效果**：
- 避免每次发送都查找 registry
- 热路径只需一次指针解引用

---

## 4. 性能预测

### 4.1 理论分析

**Throughput-First vs Latency-First**：

| Producer 数 | Latency-First | Throughput-First | 预期提升 |
|-------------|---------------|------------------|----------|
| 1P          | 153M/s        | ~150M/s          | 0.98x    |
| 2P          | 101M/s        | ~120M/s          | 1.19x    |
| 4P          | 171M/s        | ~220M/s          | 1.29x    |
| 8P          | ~280M/s       | ~420M/s          | 1.50x    |
| 16P         | ~320M/s?      | ~650M/s          | 2.03x    |

**分析**：
- **1P-2P**：Per-producer 架构的开销（轮询、链表）可能抵消收益
- **4P+**：无 CAS 竞争的优势开始显现
- **8P+**：线性扩展，Latency-First 开始出现瓶颈

### 4.2 关键指标

**吞吐量**：
- 目标：8P1C ≥ 400M/s
- 验收：≥ Latency-First 的 1.4x

**延迟**：
- P50: ~200ns（vs Latency 50ns）
- P99: ~1μs（批量带来的延迟）

**内存**：
- 固定：totalCapacity × sizeof(T)
- 无动态分配（稳态）

**可扩展性**：
- 16P/32P 仍保持线性

---

## 5. 实施计划

### 5.1 Phase 1：核心实现（已完成）

- [x] `ThroughputBoundedChannel` 类框架
- [x] `ProducerRing` 数据结构
- [x] Producer 发送路径
- [x] Consumer 接收路径
- [x] Ready-list 管理
- [x] 线程本地缓存

**文件**：
- `src/cpp/galay-kernel/concurrency/mpsc/throughput_bounded_channel.h`

### 5.2 Phase 2：本机验证（进行中）

- [x] 创建对比基准测试
- [ ] 运行性能测试
- [ ] 分析结果
- [ ] 调优参数

**基准测试**：
- `benchmark/cpp/kernel/b28_mpsc_strategy_comparison.cc`

**测试矩阵**：
```
Producers: 1, 2, 4, 8, 16
Capacity: 256, 4096
Rounds: 5 (取中位数)
```

### 5.3 Phase 3：Rust Crossbeam 对比（本周）

**目标**：验证是否达到或超越 Crossbeam

**测试环境**：
1. **本机**（Mac）：
   - 开发验证
   - 快速迭代

2. **腾讯机器**（Linux）：
   - 生产环境
   - 最终验收

**Crossbeam 基准测试**：
```bash
cd benchmark/cpp/kernel/compare/rust-channel
cargo bench --bench bounded_mpsc
```

**对比维度**：
- 吞吐量：msg/s
- 延迟：P50, P99, P999
- 扩展性：1P-16P
- 内存占用：RSS

### 5.4 Phase 4：腾讯机器验证（下周）

**环境配置**：
- CPU：（待确认）
- 核心数：（待确认）
- L1/L2/L3 缓存：（待确认）
- NUMA：（待确认）

**测试场景**：
1. 标准测试（同本机）
2. NUMA 感知测试
3. 极限压力测试（32P/64P）

### 5.5 Phase 5：API 设计与集成（下周）

**统一 API**：

```cpp
enum class ChannelStrategy {
    Auto,           // 自动选择
    Latency,        // 延迟优先
    Throughput      // 吞吐优先
};

// 方式 1：显式选择
auto latency_ch = BoundedChannel<int>(4096, ChannelStrategy::Latency);
auto throughput_ch = BoundedChannel<int>(4096, ChannelStrategy::Throughput);

// 方式 2：自动选择（默认）
auto auto_ch = BoundedChannel<int>(4096);  // 运行时检测
```

**自动策略**：
- 初始：Latency（开销低）
- 检测到 4+ concurrent producers：切换到 Throughput
- 或环境变量：`GALAY_MPSC_STRATEGY=throughput`

---

## 6. 风险与缓解

### 6.1 风险

1. **内存开销增加**
   - Per-producer ring 需要更多内存
   - 缓解：动态分配 ring，复用池

2. **低并发性能下降**
   - 1P-2P 场景下轮询开销可能抵消收益
   - 缓解：自动策略，低并发用 Latency

3. **实现复杂度**
   - 两套实现增加维护成本
   - 缓解：共享核心组件，清晰分层

4. **调优难度**
   - 批量大小、配额参数需要调优
   - 缓解：提供合理默认值，允许配置

### 6.2 兼容性

- ✅ API 向后兼容
- ✅ 行为兼容（FIFO 语义）
- ⚠️ 性能特性变化（需文档说明）

---

## 7. 测试清单

### 7.1 功能测试

- [ ] 基本收发
- [ ] 关闭语义
- [ ] Ring 满处理
- [ ] 多生产者正确性
- [ ] 批量发布正确性
- [ ] Ready-list 公平性

### 7.2 性能测试

- [ ] 1P1C 基准
- [ ] 2P1C 低竞争
- [ ] 4P1C 中竞争
- [ ] 8P1C 高竞争
- [ ] 16P1C 极限竞争
- [ ] 不同容量（64-8192）

### 7.3 压力测试

- [ ] 长时间运行（24h）
- [ ] 内存泄漏检测
- [ ] 线程安全验证
- [ ] NUMA 环境测试

### 7.4 对比测试

- [ ] vs Latency-First
- [ ] vs Crossbeam bounded
- [ ] vs Unbounded
- [ ] vs std::mpsc (Rust)

---

## 8. 下一步行动

### 8.1 立即（今天）

1. **等待测试完成**
   ```bash
   # 检查测试进度
   cat /tmp/strategy_comparison.log
   ```

2. **分析结果**
   - 吞吐量对比
   - 扩展性曲线
   - 找出瓶颈

3. **参数调优**
   - 调整批量大小
   - 调整配额
   - 测试不同容量分配策略

### 8.2 短期（本周）

1. **Rust 对比测试**
   ```bash
   cd benchmark/cpp/kernel/compare/rust-channel
   cargo bench --bench bounded_mpsc
   ```

2. **完善实现**
   - 添加 async 支持（awaitable）
   - 实现批量接收
   - 添加监控指标

3. **文档完善**
   - 使用指南
   - 性能特性说明
   - 最佳实践

### 8.3 中期（下周）

1. **腾讯机器测试**
   - 部署测试环境
   - 运行完整测试套件
   - 生成最终报告

2. **API 设计**
   - 统一策略选择
   - 自动策略实现
   - 示例代码

3. **Code Review**
   - 提交 PR
   - 团队审查
   - 合并主分支

---

## 9. 成功标准

### 9.1 必达目标

- ✅ 8P1C ≥ 400M/s（vs 当前 ~280M/s）
- ✅ 达到或超越 Crossbeam
- ✅ 功能完整、稳定

### 9.2 期望目标

- 🎯 16P1C ≥ 600M/s（线性扩展）
- 🎯 超越 Crossbeam 20%+
- 🎯 成为业界最快的 bounded MPSC

### 9.3 理想目标

- 🚀 32P1C 持续线性扩展
- 🚀 NUMA 感知优化
- 🚀 Zero-copy 批量 API

---

## 10. 总结

**核心价值**：
1. **突破性能上限**：从 171M/s → 400M/s+
2. **线性扩展性**：16P/32P 仍保持高性能
3. **架构完整性**：与 unbounded 对齐

**技术亮点**：
1. Per-producer ring 消除 CAS 竞争
2. Ready-list + 配额轮询保证公平性
3. 批量优化减少原子操作 95%

**下一步**：
- 等待测试结果
- 与 Rust Crossbeam 对比
- 腾讯机器最终验证

**预期交付时间**：1-2 周
