# Phase 2: 架构优化 (3-4 周)

## 优先级: P1 - 重要

**目标:** 简化同步机制，重构 waiter 系统，优化缓存策略

## 2.1 简化 Waiter 系统

### 问题分析

当前 pump 系统的复杂度：

```cpp
// 当前架构 (bounded_channel.h)
Producer → trySend → requestPump → runPump → drainRecvWaiters
                                           → drainSendWaiters

Consumer → recv → registerWaiter → pump 认领 → 值交换 → 延迟唤醒
```

**问题点:**
1. Pump 是额外的竞争点
2. Waiter 队列需要动态分配
3. Deferred wake 增加复杂度
4. 多次 CAS 状态转换

### 优化方案: 直接唤醒模型

**新架构:**
```cpp
// 简化架构
Producer → trySend → 直接检查 waiter → 原地唤醒
Consumer → recv → 注册 → 被唤醒 → 直接取值
```

**实现要点:**

#### 2.1.1 单 Waiter 槽位

```cpp
// 替代 WaiterQueue，使用单槽位
struct WaiterSlot {
    std::atomic<WaiterState*> waiter{nullptr};
    std::atomic<uint8_t> phase{0};  // 0=empty, 1=arming, 2=armed
};

class BoundedChannel {
private:
    alignas(128) WaiterSlot m_recvWaiter;
    alignas(128) WaiterSlot m_sendWaiter;
};
```

**优点:**
- 无动态分配
- 最多一个等待者，简化逻辑
- 更清晰的所有权语义

**限制:**
- 不支持多个并发 recv/send awaiter
- 需要文档说明这个限制

#### 2.1.2 简化唤醒逻辑

```cpp
bool tryWakeRecvWaiter() noexcept {
    uint8_t expected = 2;  // armed
    if (!m_recvWaiter.phase.compare_exchange_strong(
            expected, 0, std::memory_order_acq_rel)) {
        return false;  // 没有等待者或正在 arming
    }

    WaiterState* waiter = m_recvWaiter.waiter.exchange(
        nullptr, std::memory_order_acq_rel);

    if (waiter != nullptr) {
        // 直接唤醒，无延迟
        waiter->waker.wakeUp();
        return true;
    }

    return false;
}
```

**收益:**
- 减少 50% 原子操作
- 消除 pump 开销
- 降低延迟

### 2.1.3 移除 Pump 系统

**变更清单:**

1. **删除代码:**
   - `runPump()` (行 1351-1375)
   - `requestPump()` (行 1301-1320)
   - `drainRecvWaiters()` (行 1322-1334)
   - `drainSendWaiters()` (行 1336-1348)
   - `WaiterQueue` 类 (行 736-806)
   - `m_pumpState` (行 1389)

2. **简化代码:**
   - `trySend()` 直接调用 `tryWakeRecvWaiter()`
   - `tryRecv()` 直接调用 `tryWakeSendWaiter()`
   - 移除 waiter 计数

3. **保留功能:**
   - Timeout 支持
   - Close 通知
   - FIFO 保证

**代码量变化:**
- 删除: ~800 行
- 新增: ~200 行
- 净减少: ~600 行

**预期收益:**
- 有界通道: +15-20% 吞吐
- 延迟降低: -30-40%

## 2.2 优化 TLS 缓存机制

### 2.2.1 双层缓存策略

**设计:**

```cpp
// 一级缓存: 单槽快速路径
struct L1Cache {
    const UnboundedChannel* channel = nullptr;
    ProducerStream* stream = nullptr;
    uint64_t generation = 0;
};

// 二级缓存: 小型哈希表
struct L2Cache {
    static constexpr size_t CAPACITY = 8;

    struct Entry {
        const UnboundedChannel* channel = nullptr;
        ProducerStream* stream = nullptr;
        ProducerLifetime* lifetime = nullptr;
        uint64_t generation = 0;
    };

    std::array<Entry, CAPACITY> entries;

    size_t hash(const UnboundedChannel* ch) const noexcept {
        return (reinterpret_cast<uintptr_t>(ch) >> 6) % CAPACITY;
    }

    ProducerStream* lookup(const UnboundedChannel* ch, uint64_t gen) noexcept {
        size_t idx = hash(ch);
        Entry& entry = entries[idx];

        if (entry.channel == ch && entry.generation == gen) {
            // 验证有效性
            if (entry.lifetime->state.load(std::memory_order_relaxed) ==
                ProducerLifetimeState::kOwned) {
                return entry.stream;
            }
        }

        return nullptr;
    }

    void insert(const UnboundedChannel* ch, ProducerStream* s,
                ProducerLifetime* lt, uint64_t gen) noexcept {
        size_t idx = hash(ch);
        entries[idx] = {ch, s, lt, gen};
    }
};
```

**查找流程:**

```cpp
ProducerStream* defaultProducerStream() noexcept {
    static thread_local L1Cache l1;
    static thread_local L2Cache l2;

    // L1 查找 (单条指令)
    if (l1.channel == this && l1.generation == m_generation) {
        return l1.stream;
    }

    // L2 查找 (哈希表)
    ProducerStream* stream = l2.lookup(this, m_generation);
    if (stream != nullptr) {
        // 升级到 L1
        l1 = {this, stream, m_generation};
        return stream;
    }

    // L3: 慢路径分配
    stream = acquireProducerStream();
    if (stream != nullptr) {
        l2.insert(this, stream, stream->lifetime, m_generation);
        l1 = {this, stream, m_generation};
    }

    return stream;
}
```

**性能特征:**
- L1 命中 (单 channel): ~2 cycles
- L2 命中 (多 channel): ~10 cycles
- L3 miss: ~100-500 cycles

**预期收益:**
- 单 channel: +5% (已优化)
- 5 channels: +15-20%
- 20 channels: +40-50%

### 2.2.2 主动清理策略

**问题:** 当前只在查找时被动清理失效条目

**优化:** 周期性清理

```cpp
struct CacheCleanup {
    size_t operations = 0;

    void maybeCleanup(L2Cache& cache) noexcept {
        if (++operations % 1024 == 0) {
            for (auto& entry : cache.entries) {
                if (entry.lifetime != nullptr &&
                    entry.lifetime->state.load(std::memory_order_relaxed) ==
                        ProducerLifetimeState::kDetached) {
                    // 清理失效条目
                    entry = {};
                }
            }
        }
    }
};
```

## 2.3 批量操作优化

### 2.3.1 有界通道批量发送

**当前问题:**
- `sendBatch` 仍然逐个检查 slot
- 没有专门的批量发布路径

**优化实现:**

```cpp
template <typename Iterator>
bool trySendBatch(Iterator begin, Iterator end) noexcept {
    const size_t count = std::distance(begin, end);
    if (count == 0) return true;

    // 一次性预留
    size_t tail = m_tail.load(std::memory_order_relaxed);
    for (;;) {
        if ((tail & kTailClosedBit) != 0) {
            return false;
        }

        // 检查是否有足够空间
        const size_t head = m_head.load(std::memory_order_acquire);
        const size_t available = m_capacity - (tail - head);
        if (available < count) {
            return false;  // 空间不足，整批失败
        }

        // 原子认领 count 个 slot
        if (m_tail.compare_exchange_weak(
                tail, tail + count,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            break;
        }
    }

    // 批量构造 (无竞争)
    size_t position = tail;
    for (auto it = begin; it != end; ++it) {
        Slot& slot = m_slots[position & m_mask];
        std::construct_at(slot.value(), std::move(*it));
        ++position;
    }

    // 批量发布 (按序 release)
    position = tail;
    for (size_t i = 0; i < count - 1; ++i) {
        Slot& slot = m_slots[position & m_mask];
        slot.sequence.store(position + 1, std::memory_order_relaxed);
        ++position;
    }

    // 最后一个用 release 发布整批
    Slot& lastSlot = m_slots[position & m_mask];
    lastSlot.sequence.store(position + 1, std::memory_order_release);

    // 只检查一次等待者
    tryWakeRecvWaiter();

    return true;
}
```

**预期收益:**
- 批量 100: +50-60% 吞吐
- 批量 1000: +80-100% 吞吐

### 2.3.2 无界通道批量优化

**关键优化:**
- 一次性预留多个 block
- 批量构造无中间检查
- 批量发布摊销 gate 开销

```cpp
bool sendBatchToStream(ProducerStream& stream,
                       std::vector<T>&& values) noexcept {
    if (!beginSend(stream)) {
        return false;
    }

    // 一次性预留所有需要的 slot
    if (!reserveProducerSlots(stream, values.size())) {
        finishSend(stream);
        return false;
    }

    // 批量构造 (无中间原子操作)
    ProducerCursor& cursor = stream.producer;
    for (T& value : values) {
        if (cursor.index == kBlockCapacity) {
            cursor.block = cursor.block->next.load(std::memory_order_relaxed);
            cursor.index = 0;
        }
        std::construct_at(
            cursor.block->slots[cursor.index].constructionAddress(),
            std::move(value));
        ++cursor.index;
    }

    // 批量发布
    publishStreamBatch(stream, values.size());
    finishSend(stream);

    return true;
}
```

**预期收益:**
- 批量 100: +40-50% 吞吐
- 批量 1000: +70-90% 吞吐

## 2.4 测试计划

### 2.4.1 单元测试

**新增测试:**

```cpp
// 测试简化的 waiter 系统
TEST(BoundedChannel, SingleWaiterModel) {
    BoundedChannel<int> ch(4);

    // 只能有一个 recv waiter
    auto r1 = ch.recv();
    auto r2 = ch.recv();  // 应该立即返回错误

    // 发送唤醒
    ch.send(42);
    EXPECT_EQ(co_await r1, 42);
}

// 测试 TLS 缓存
TEST(UnboundedChannel, TLSCacheMultiChannel) {
    std::vector<UnboundedChannel<int>> channels(20);

    // 热身缓存
    for (auto& ch : channels) {
        EXPECT_TRUE(ch.send(1));
    }

    // 测量性能
    auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < 1000; ++i) {
        for (auto& ch : channels) {
            ch.send(i);
        }
    }
    auto elapsed = std::chrono::steady_clock::now() - start;

    // 应该快于 Phase 1
}
```

### 2.4.2 基准测试

**对比维度:**
- Phase 1 vs Phase 2
- 不同批量大小
- 不同 channel 数量

**验收标准:**
- ✅ 相比 Phase 1 提升 20-30%
- ✅ 批量操作提升 50-100%
- ✅ 多 channel 场景提升 40-60%
- ✅ 所有正确性测试通过

## Phase 2 时间表

| 周 | 任务 | 负责人 | 状态 |
|----|------|--------|------|
| W4 | 简化 waiter 系统设计 | TBD | 待开始 |
| W4-5 | 实现单槽位 waiter | TBD | 待开始 |
| W5 | 移除 pump 系统 | TBD | 待开始 |
| W6 | 双层 TLS 缓存 | TBD | 待开始 |
| W6 | 批量操作优化 | TBD | 待开始 |
| W7 | 完整测试和验证 | TBD | 待开始 |
