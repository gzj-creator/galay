# MPSC 通道双策略架构设计

## 1. 背景

### 1.1 当前问题

**Bounded MPSC 性能断崖**：
- 2P1C: ~28.5M/s
- 4P1C: ~6.9M/s (断崖)
- 8P1C: ~5.46M/s (持续下降)

**根本原因**：
- 所有 producer 竞争单一 `m_tail` 原子变量
- 简单 spin + yield 退避在高并发下无效
- CAS 竞争风暴导致缓存行乒乓效应

**对比 Unbounded**：
- 4P1C: Crossbeam 的 2.85x
- 8P1C: Crossbeam 的 5.06x
- 关键：Per-producer stream 消除共享 tail CAS

### 1.2 隔离实验验证

**指数退避实验**（仅修改退避策略）：

| 场景  | 当前实现  | 指数退避  | 提升   |
|-------|-----------|-----------|--------|
| 2P1C  | 28.5M/s   | 36.5M/s   | 1.28x  |
| 4P1C  | 6.9M/s    | 38.1M/s   | 5.52x  |
| 8P1C  | 5.46M/s   | 51.9M/s   | 9.51x  |

**结论**：
- 退避策略改进可显著提升高并发性能
- 但仍未达到 per-producer 架构的理论上限
- 需要结合架构级改进

---

## 2. 双策略设计

### 2.1 策略概览

#### **策略 1：吞吐优先（Throughput-First）**

**设计目标**：
- 最大化多生产者高并发吞吐量
- 消除共享 tail CAS 竞争
- 优化批量处理性能

**核心特性**：
1. **Per-Producer RingBuffer**
   - 每个 producer 独占一个小 ring（容量 = 总容量 / 预期 producer 数）
   - 无共享 tail CAS，producer 热路径只写本地变量
   - 每个 ring 独立的 head/tail/sequence 数组

2. **Ready-List 管理**
   - 有数据的 ring 加入 ready list（类似 unbounded）
   - Consumer 按配额轮询避免饥饿
   - 批量发布 + 批量消费减少原子操作

3. **批量优化**
   - Producer batch publish（减少 ready-list 更新）
   - Consumer batch drain（减少轮询开销）
   - 分批更新 head 指针（减少同步频率）

**权衡**：
- ✅ 高并发吞吐量极高（4P/8P 场景）
- ✅ 可扩展性好（producer 数增加性能不断崖）
- ❌ 单条消息延迟稍高（批量带来的延迟）
- ❌ 内存占用稍高（每个 producer 独立 ring）

---

#### **策略 2：延迟优先（Latency-First）**

**设计目标**：
- 最小化单条消息端到端延迟
- 保持低并发场景的高性能
- 简化实现，减少开销

**核心特性**：
1. **共享 RingBuffer + 指数退避**
   - 保持当前单 ring 架构
   - 改进退避策略：1, 2, 4, 8, ... 64 次 CPU pause
   - 2P/4P 场景下显著减少 CAS 冲突

2. **快速路径优化**
   - 单条消息直接通知（无批量延迟）
   - Waiter 立即唤醒（无配额轮询）
   - 热路径内联优化

3. **自适应优化**
   - 检测竞争强度动态调整退避
   - 低竞争时快速路径，高竞争时指数退避

**权衡**：
- ✅ 单条消息延迟最低（1P/2P 场景）
- ✅ 实现简单，维护成本低
- ✅ 内存占用低（单 ring）
- ❌ 高并发扩展性受限（4P+ 性能受 CAS 限制）
- ❌ 公平性略差（无轮询机制）

---

## 3. 详细设计

### 3.1 吞吐优先（Throughput-First）

#### 3.1.1 数据结构

```cpp
template <BoundedValue T>
class ThroughputBoundedChannel {
private:
    struct ProducerRing {
        alignas(kCacheLineSize) std::atomic<size_t> tail{0};
        alignas(kCacheLineSize) std::atomic<size_t> head{0};
        alignas(kCacheLineSize) std::atomic<bool> active{false};
        
        size_t capacity;
        size_t mask;
        std::vector<Slot> slots;
        
        ProducerRing* readyNext = nullptr;
        std::atomic<uint32_t> refCount{2}; // registry + owner
    };
    
    struct ProducerHandle {
        ProducerRing* ring;
        uint64_t generation;
        size_t localTail = 0;
        size_t batchCount = 0;
        static constexpr size_t PUBLISH_BATCH = 8;
    };
    
    // Registry
    alignas(kCacheLineSize) std::atomic<ProducerRing*> m_ringHead{nullptr};
    
    // Ready list (consumer side)
    alignas(kCacheLineSize) std::atomic<ProducerRing*> m_readyStack{nullptr};
    ProducerRing* m_readyHead = nullptr;
    ProducerRing* m_readyTail = nullptr;
    size_t m_activeRingCount = 0;
    size_t m_quotaUsed = 0;
    
    static constexpr size_t kConsumerQuota = 64;
    static constexpr size_t kMinRingCapacity = 64;
};
```

#### 3.1.2 Producer 发送流程

```cpp
// 1. 获取/创建 producer ring (线程本地缓存)
ProducerHandle* getProducerHandle() {
    thread_local ProducerHandle* handle = nullptr;
    if (!handle || handle->generation != m_generation) {
        handle = acquireProducerRing();
    }
    return handle;
}

// 2. 发送单条消息（累积批量发布）
bool send(T&& value) {
    ProducerHandle* handle = getProducerHandle();
    
    // a. 无 CAS 认领 slot
    size_t position = handle->localTail++;
    size_t index = position & handle->ring->mask;
    Slot& slot = handle->ring->slots[index];
    
    // b. 等待 slot 可用（消费者释放）
    while (slot.sequence.load(acquire) != position) {
        cpuPause();
    }
    
    // c. 写入值
    construct_at(slot.value(), std::move(value));
    slot.sequence.store(position + 1, release);
    
    // d. 批量发布
    if (++handle->batchCount >= PUBLISH_BATCH) {
        publishBatch(handle);
    }
    
    return true;
}

// 3. 批量发布（减少原子操作）
void publishBatch(ProducerHandle* handle) {
    // 发布 tail 指针
    handle->ring->tail.store(handle->localTail, release);
    
    // 激活 ready list
    if (!handle->ring->active.exchange(true, acq_rel)) {
        pushReadyRing(handle->ring);
    }
    
    handle->batchCount = 0;
    
    // 唤醒 waiter
    if (m_waiterCount.load(seq_cst) != 0) {
        requestPump();
    }
}
```

#### 3.1.3 Consumer 接收流程

```cpp
std::optional<T> tryRecv() {
    // 1. 刷新 ready list
    if (m_readyHead == nullptr || m_quotaUsed == 0) {
        appendReadyRings();
    }
    
    // 2. 轮询 active rings（配额限制）
    size_t remaining = m_activeRingCount;
    while (m_readyHead != nullptr && remaining > 0) {
        ProducerRing* ring = m_readyHead;
        
        // a. 尝试从当前 ring 消费
        auto value = tryPopRing(ring);
        if (value.has_value()) {
            m_quotaUsed++;
            if (m_quotaUsed >= kConsumerQuota) {
                rotateReadyHead(); // 公平性
            }
            return value;
        }
        
        // b. 当前 ring 空，轮转到下一个
        rotateReadyHead();
        remaining--;
    }
    
    return std::nullopt;
}

std::optional<T> tryPopRing(ProducerRing* ring) {
    size_t position = ring->head.load(relaxed);
    size_t index = position & ring->mask;
    Slot& slot = ring->slots[index];
    
    if (slot.sequence.load(acquire) != position + 1) {
        ring->active.store(false, release); // 标记为空
        return std::nullopt;
    }
    
    T value = std::move(*slot.value());
    destroy_at(slot.value());
    slot.sequence.store(position + ring->capacity, release);
    ring->head.store(position + 1, release);
    
    return value;
}
```

---

### 3.2 延迟优先（Latency-First）

#### 3.2.1 指数退避实现

```cpp
class ExponentialBackoff {
private:
    uint32_t m_step = 0;
    static constexpr uint32_t kSpinLimit = 6;   // 2^6 = 64 spins
    static constexpr uint32_t kYieldLimit = 10; // then yield
    
public:
    // CAS 竞争退避
    void backoffOnCAS() {
        if (m_step <= kSpinLimit) {
            const uint32_t spins = 1U << m_step;
            for (uint32_t i = 0; i < spins; ++i) {
                cpuPause();
            }
            m_step++;
        } else {
            std::this_thread::yield();
            if (m_step < kYieldLimit) {
                m_step++;
            }
        }
    }
    
    // Slot 等待退避（更激进）
    void backoffOnSlot() {
        if (m_step <= kSpinLimit) {
            const uint32_t spins = 1U << std::min(m_step, 3U); // 最多 8 spins
            for (uint32_t i = 0; i < spins; ++i) {
                cpuPause();
            }
            m_step++;
        } else {
            std::this_thread::yield();
        }
    }
    
    void reset() { m_step = 0; }
};
```

#### 3.2.2 改进的 enqueue 流程

```cpp
RingEnqueueResult ringEnqueueResult(T&& value) noexcept {
    size_t tail = m_tail.load(relaxed);
    ExponentialBackoff backoff;
    
    for (;;) {
        // 1. 检查关闭
        if ((tail & kTailClosedBit) != 0) {
            return RingEnqueueResult::kClosed;
        }
        
        // 2. 获取 slot
        size_t position = tail & kTailPositionMask;
        Slot& slot = m_slots[position & m_mask];
        size_t sequence = slot.sequence.load(acquire);
        
        SignedSize diff = static_cast<SignedSize>(sequence - position);
        
        if (diff == 0) {
            // 3. CAS 认领 slot
            if (m_tail.compare_exchange_weak(tail, position + 1,
                                             relaxed, relaxed)) {
                // 成功：写入值
                construct_at(slot.value(), std::move(value));
                slot.sequence.store(position + 1, seq_cst);
                return RingEnqueueResult::kSent;
            }
            // 失败：指数退避
            backoff.backoffOnCAS();
            
        } else if (diff < 0) {
            // 4. Ring 满
            return RingEnqueueResult::kFull;
            
        } else {
            // 5. 观察到更新的 tail，重新加载
            tail = m_tail.load(relaxed);
            backoff.backoffOnSlot();
        }
    }
}
```

---

## 4. API 设计

### 4.1 构造函数选择策略

```cpp
// 策略 1：吞吐优先（显式构造）
auto channel = mpsc::ThroughputBoundedChannel<int>(
    4096,                    // 总容量
    8,                       // 预期最大 producer 数
    64                       // 每 ring 最小容量
);

// 策略 2：延迟优先（显式构造）
auto channel = mpsc::LatencyBoundedChannel<int>(
    4096                     // 容量
);

// 默认策略：根据场景自动选择
auto channel = mpsc::BoundedChannel<int>(
    4096,                    // 容量
    ChannelStrategy::Auto    // 自动检测
);
```

### 4.2 自动策略选择

```cpp
enum class ChannelStrategy {
    Auto,           // 运行时检测
    Throughput,     // 显式吞吐优先
    Latency         // 显式延迟优先
};

// Auto 策略逻辑：
// - 初始使用 Latency（低开销）
// - 检测到 4+ concurrent producers 时切换到 Throughput
// - 或构造时通过环境变量/配置指定
```

---

## 5. 实现计划

### 5.1 阶段 1：延迟优先（快速胜利）

**目标**：2-4 工作日
- ✅ 实现指数退避 backoff
- ✅ 集成到现有 bounded_channel.h
- ✅ 基准测试验证 2P/4P/8P 提升
- ✅ 与 Crossbeam 对比

**预期收益**：
- 4P1C: 6.9M → ~38M (5x)
- 8P1C: 5.46M → ~52M (9x)

### 5.2 阶段 2：吞吐优先（架构升级）

**目标**：1-2 周
- 实现 per-producer ring 架构
- Ready-list 管理
- Producer token + TLS 缓存
- 批量发布/消费优化

**预期收益**：
- 4P1C: ~60M+ (接近 unbounded 水平)
- 8P1C: ~80M+ (线性扩展)

### 5.3 阶段 3：统一 API

**目标**：3-5 工作日
- 策略选择 API
- 自动策略检测
- 文档和示例
- 迁移指南

---

## 6. 性能预测

### 6.1 延迟优先（指数退避）

| 场景  | 当前     | 预期      | 提升  |
|-------|----------|-----------|-------|
| 1P1C  | ~45M/s   | ~45M/s    | 1.0x  |
| 2P1C  | ~28.5M/s | ~36M/s    | 1.3x  |
| 4P1C  | ~6.9M/s  | ~38M/s    | 5.5x  |
| 8P1C  | ~5.46M/s | ~52M/s    | 9.5x  |

### 6.2 吞吐优先（per-producer ring）

| 场景  | Crossbeam | 预期      | 倍数  |
|-------|-----------|-----------|-------|
| 1P1C  | ~35M/s    | ~40M/s    | 1.1x  |
| 2P1C  | ~30M/s    | ~50M/s    | 1.7x  |
| 4P1C  | ~25M/s    | ~70M/s    | 2.8x  |
| 8P1C  | ~18M/s    | ~90M/s    | 5.0x  |

### 6.3 延迟对比

| 策略      | P50 延迟 | P99 延迟 | 适用场景        |
|-----------|----------|----------|-----------------|
| Latency   | ~50ns    | ~200ns   | 1P-2P 实时系统  |
| Throughput| ~200ns   | ~1us     | 4P+ 高吞吐系统  |

---

## 7. 风险与缓解

### 7.1 风险

1. **内存开销增加**（Throughput 策略）
   - Per-producer ring 需要额外内存
   - 缓解：动态分配 + ring 复用

2. **公平性问题**（Latency 策略）
   - 无轮询可能导致某些 producer 饥饿
   - 缓解：检测饥饿并降级到 Throughput

3. **实现复杂度**
   - 两套实现增加维护成本
   - 缓解：共享核心组件，清晰分层

### 7.2 兼容性

- ✅ API 向后兼容（默认 Latency 策略）
- ✅ 行为兼容（保持 FIFO 语义）
- ⚠️ 性能特性变化（需要文档说明）

---

## 8. 总结

**双策略设计的核心价值**：
1. **覆盖全场景**：1P-8P 都有最优策略
2. **权衡透明**：用户可根据需求显式选择
3. **渐进式演进**：先实现快速胜利，再架构升级

**下一步**：
1. ✅ 实现阶段 1（延迟优先 + 指数退避）
2. 基准测试验证
3. Code Review
4. 合并主分支
5. 启动阶段 2（吞吐优先架构）
