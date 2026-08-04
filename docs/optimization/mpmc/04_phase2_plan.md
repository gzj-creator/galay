# MPMC Phase 2: 架构简化优化 (3-4 周)

## 优先级: P1 - 重要

**目标:** 简化 Pump 系统，优化 Waiter 管理，引入专用批量路径

**预期收益:**
- 有界通道: 额外 +15-20% 吞吐
- 无界通道: 额外 +10-15% 吞吐

## 2.1 简化 Waiter 系统

### 问题分析

**当前架构复杂度:**

```cpp
// 多个 waiter 队列
moodycamel::ConcurrentQueue<WaiterPtr> m_recvWaiters;
moodycamel::ConcurrentQueue<WaiterPtr> m_sendWaiters;

// 多个状态原子变量
std::atomic<uint8_t> m_pumpState;
std::atomic<bool> m_recvWaiterPathUsed;
std::atomic<bool> m_sendWaiterPathUsed;

// 复杂的 pump 系统
void runPump(uint8_t work) {
    // 从队列取 waiter
    // 检查状态
    // 尝试满足
    // 重新入队或唤醒
}
```

**开销来源:**
1. Moodycamel 队列的入队/出队开销
2. Pump 状态机复杂度
3. 多个原子标志的检查
4. Waiter 的动态分配

### 优化策略 1: 轻量级 Waiter 栈

**设计理念:**
- 用简单的侵入式栈替代 moodycamel 队列
- 减少动态分配
- 简化 pump 逻辑

**实现:**

```cpp
// 侵入式 waiter 节点
template <typename T>
struct WaiterNode {
    WaiterNode* next{nullptr};
    Waker waker;
    std::atomic<uint8_t> state{0};  // 0=armed, 1=fulfilled, 2=cancelled

    enum State : uint8_t {
        kArmed = 0,
        kFulfilled = 1,
        kCancelled = 2,
    };
};

// 侵入式栈
template <typename T>
class WaiterStack {
public:
    void push(WaiterNode<T>* node) noexcept {
        node->next = m_head.load(std::memory_order_relaxed);
        while (!m_head.compare_exchange_weak(
                node->next, node,
                std::memory_order_release,
                std::memory_order_relaxed)) {
            // CAS 重试
        }
    }

    WaiterNode<T>* popAll() noexcept {
        return m_head.exchange(nullptr, std::memory_order_acquire);
    }

    bool empty() const noexcept {
        return m_head.load(std::memory_order_relaxed) == nullptr;
    }

private:
    std::atomic<WaiterNode<T>*> m_head{nullptr};
};
```

**优势:**
- 零动态分配（waiter 在协程栈上）
- Push: 单次 CAS
- PopAll: 单次 exchange
- 更简单的所有权语义

**权衡:**
- 不保证 FIFO（栈是 LIFO）
- 需要验证是否影响公平性

### 优化策略 2: 直接唤醒模型

**问题:** 当前 pump 系统引入延迟

**新模型:**

```cpp
// 发送路径
bool trySend(T&& value) {
    // 1. 尝试直接放入 ring
    if (ringEnqueue(std::move(value)) == RingEnqueueResult::kSent) {
        // 2. 尝试唤醒一个接收者
        tryWakeOneRecvWaiter();
        return true;
    }

    return false;
}

// 直接唤醒
void tryWakeOneRecvWaiter() noexcept {
    if (m_recvWaiters.empty()) {
        return;
    }

    // Pop 一个 waiter
    WaiterNode<T>* waiter = m_recvWaiters.popOne();
    if (waiter == nullptr) {
        return;
    }

    // 尝试标记为 fulfilled
    uint8_t expected = WaiterNode<T>::kArmed;
    if (waiter->state.compare_exchange_strong(
            expected, WaiterNode<T>::kFulfilled,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        // 成功认领，直接唤醒
        waiter->waker.wakeUp();
    }
    // 失败表示已被取消，无需处理
}
```

**优势:**
- 消除 pump 系统
- 减少延迟
- 更简单的控制流

**预期收益:** +10-15% 吞吐，-20-30% 延迟

### 优化策略 3: 批量唤醒

**场景:** 批量发送后唤醒多个等待者

```cpp
void tryWakeBatchRecvWaiters(size_t maxCount) noexcept {
    if (m_recvWaiters.empty()) {
        return;
    }

    // Pop 多个 waiter
    WaiterNode<T>* head = m_recvWaiters.popAll();
    WaiterNode<T>* current = head;
    size_t woken = 0;

    while (current != nullptr && woken < maxCount) {
        WaiterNode<T>* next = current->next;

        uint8_t expected = WaiterNode<T>::kArmed;
        if (current->state.compare_exchange_strong(
                expected, WaiterNode<T>::kFulfilled,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            current->waker.wakeUp();
            ++woken;
        }

        current = next;
    }

    // 剩余的 waiter 重新入队
    if (current != nullptr) {
        m_recvWaiters.pushAll(current);
    }
}
```

**使用场景:**
```cpp
size_t trySendBatch(std::span<T> values) {
    size_t sent = 0;
    for (T& value : values) {
        if (ringEnqueue(std::move(value)) == RingEnqueueResult::kSent) {
            ++sent;
        } else {
            break;
        }
    }

    if (sent > 0) {
        // 批量唤醒
        tryWakeBatchRecvWaiters(sent);
    }

    return sent;
}
```

**预期收益:** 批量场景 +20-30% 吞吐

## 2.2 优化无界通道包装层

### 问题: Moodycamel 包装开销

**当前实现:**
```cpp
bool trySend(T&& value) {
    // 1. 检查关闭
    if (isClosed()) return false;

    // 2. 调用 moodycamel
    bool success = m_queue.enqueue(std::move(value));
    if (!success) return false;

    // 3. 检查 waiter
    if (m_recvWaiterState.load(...) & kRecvWaiterPathUsed) {
        requestPump(kRecvWork);
    }

    return true;
}
```

**优化: 条件编译快速路径**

```cpp
// 编译期选择
#ifdef GALAY_MPMC_UNBOUNDED_FAST_PATH
    // 快速路径：假设不关闭，不检查 waiter
    bool trySendFast(T&& value) {
        return m_queue.enqueue(std::move(value));
    }
#endif

// 完整路径
bool trySend(T&& value) {
    if (isClosed()) [[unlikely]] {
        return false;
    }

    bool success = m_queue.enqueue(std::move(value));
    if (!success) [[unlikely]] {
        return false;
    }

    // 使用 Phase 1 的 relaxed + fence 优化
    if (m_recvWaiterState.load(std::memory_order_relaxed) & kRecvWaiterPathUsed) {
        [[unlikely]] wakeRecvWaiters();
    }

    return true;
}
```

**使用 [[likely]] / [[unlikely]] 属性:**
- 帮助编译器优化分支预测
- 快速路径内联
- 慢路径外联

**预期收益:** +3-5% 吞吐

### 策略: Token 路径优化

**问题:** Token 验证仍有开销

**优化: 假设 Token 有效**

```cpp
// 假设 token 有效的快速路径
bool sendTokenUnchecked(ProducerToken& token, T&& value) {
    // 直接调用 moodycamel token API
    return m_queue.enqueue(token.m_moodycamelToken, std::move(value));
}

// 安全的包装
bool send(ProducerToken& token, T&& value) {
    // 调试模式：检查
    #ifndef NDEBUG
        if (!token.validFor(*this)) {
            return false;
        }
    #endif

    // 发布模式：假设有效
    bool success = sendTokenUnchecked(token, std::move(value));

    // 发送后才检查 waiter
    if (success &&
        m_recvWaiterState.load(std::memory_order_relaxed) & kRecvWaiterPathUsed) {
        [[unlikely]] wakeRecvWaiters();
    }

    return success;
}
```

**权衡:**
- 发布模式更快
- 调试模式保持安全
- 用户需要保证 token 有效性

**预期收益:** Token 路径 +5-8% 吞吐

## 2.3 专用批量路径

### 有界通道批量优化

**当前问题:**
- 批量操作是逐个调用 `trySend`
- 每次都检查 waiter
- 没有批量 CAS 优化

**优化实现:**

```cpp
template <typename Iterator>
size_t trySendBatch(Iterator begin, Iterator end) {
    const size_t count = std::distance(begin, end);
    if (count == 0) return 0;

    // 1. 批量预留 slot
    uint64_t tail = m_tail.load(std::memory_order_relaxed);
    uint64_t startPosition = 0;

    for (;;) {
        if ((tail & kTailClosedBit) != 0) {
            return 0;
        }

        // 检查是否有足够空间
        const uint64_t head = m_head.load(std::memory_order_acquire);
        const uint64_t available = m_capacity - (tail - head);

        // 批量认领（最多可用数量）
        const size_t toSend = std::min(count, static_cast<size_t>(available));
        if (toSend == 0) {
            return 0;
        }

        if (m_tail.compare_exchange_weak(
                tail, tail + toSend,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            startPosition = tail;
            break;
        }
    }

    // 2. 批量构造（无竞争）
    size_t position = startPosition;
    size_t sent = 0;
    for (auto it = begin; it != end && sent < count; ++it, ++sent) {
        Slot& slot = m_slots[position & m_mask];
        std::construct_at(slot.value(), std::move(*it));
        ++position;
    }

    // 3. 批量发布（按序 release）
    position = startPosition;
    for (size_t i = 0; i < sent; ++i) {
        Slot& slot = m_slots[position & m_mask];

        // 前 N-1 个用 relaxed
        if (i < sent - 1) {
            slot.sequence.store(position + 1, std::memory_order_relaxed);
        } else {
            // 最后一个用 release 发布整批
            slot.sequence.store(position + 1, std::memory_order_release);
        }

        ++position;
    }

    // 4. 批量唤醒接收者
    if (sent > 0) {
        tryWakeBatchRecvWaiters(sent);
    }

    return sent;
}
```

**关键优化点:**
1. 单次 CAS 认领多个 slot
2. 批量构造无中间检查
3. 只最后一个 slot 用 release
4. 批量唤醒摊销 waiter 开销

**预期收益:**
- 批量 10: +30-40%
- 批量 100: +60-80%
- 批量 1000: +100-150%

### 无界通道批量优化

**策略: 直接使用 Moodycamel 的批量 API**

```cpp
template <typename Iterator>
size_t trySendBatch(Iterator begin, Iterator end) {
    const size_t count = std::distance(begin, end);
    if (count == 0) return 0;

    if (isClosed()) [[unlikely]] {
        return 0;
    }

    // 使用 moodycamel 的 enqueue_bulk
    size_t sent = m_queue.enqueue_bulk(std::make_move_iterator(begin), count);

    // 批次结束统一检查 waiter
    if (sent > 0 &&
        m_recvWaiterState.load(std::memory_order_relaxed) & kRecvWaiterPathUsed) {
        [[unlikely]] tryWakeBatchRecvWaiters(sent);
    }

    return sent;
}

// 批量接收
template <typename OutputIterator>
size_t tryRecvBatch(OutputIterator dest, size_t maxCount) {
    // 使用 moodycamel 的 try_dequeue_bulk
    size_t received = m_queue.try_dequeue_bulk(dest, maxCount);

    // 通知发送等待者
    if (received > 0 &&
        m_sendWaiterState.load(std::memory_order_relaxed) & kSendWaiterPathUsed) {
        [[unlikely]] tryWakeBatchSendWaiters(received);
    }

    return received;
}
```

**预期收益:**
- 批量 10: +25-35%
- 批量 100: +50-70%
- 批量 1000: +80-120%

## 2.4 MPMC 特有优化

### 优化: 减少 Head/Tail 缓存行争用

**当前布局:**
```cpp
alignas(128) std::atomic<uint64_t> m_tail{0};
alignas(128) std::atomic<uint64_t> m_head{0};
```

**问题:**
- 128B 对齐已经避免 false sharing
- 但 head/tail 仍在不同核心间跳跃

**优化: Padding 隔离**

```cpp
struct alignas(128) ProducerPad {
    std::atomic<uint64_t> tail{0};
    char padding[128 - sizeof(std::atomic<uint64_t>)];
};

struct alignas(128) ConsumerPad {
    std::atomic<uint64_t> head{0};
    char padding[128 - sizeof(std::atomic<uint64_t>)];
};

class BoundedChannel {
private:
    ProducerPad m_producerPad;  // 独立缓存行
    ConsumerPad m_consumerPad;  // 独立缓存行
    // ... 其他字段
};
```

**预期收益:** +2-5% (边际改进)

### 优化: 多消费者公平性

**问题:** LIFO 栈可能导致饥饿

**策略: 定期重排**

```cpp
class WaiterStack {
    std::atomic<uint64_t> m_popCount{0};
    static constexpr uint64_t kReorderInterval = 100;

    WaiterNode<T>* popOne() {
        const uint64_t count = m_popCount.fetch_add(1, std::memory_order_relaxed);

        // 定期 pop all 然后反转
        if ((count % kReorderInterval) == 0) {
            WaiterNode<T>* head = popAll();
            if (head != nullptr) {
                // 反转链表
                head = reverseList(head);
                // 保留第一个，其余推回
                WaiterNode<T>* rest = head->next;
                if (rest != nullptr) {
                    pushAll(rest);
                }
                return head;
            }
        }

        // 正常 pop
        return popOneNormal();
    }
};
```

**权衡:**
- 增加公平性
- 轻微性能开销
- 可配置间隔

## 2.5 测试和验证

### 功能测试

**新增测试用例:**

```cpp
// 测试简化的 waiter 系统
TEST(MPMCBounded, SimplifiedWaiterWakeup) {
    BoundedChannel<int> ch(4);

    // 多个接收者等待
    std::vector<std::thread> receivers;
    for (int i = 0; i < 4; ++i) {
        receivers.emplace_back([&ch] {
            auto value = ch.recv();
            EXPECT_TRUE(value.has_value());
        });
    }

    // 批量发送唤醒
    std::vector<int> values = {1, 2, 3, 4};
    EXPECT_EQ(ch.trySendBatch(values.begin(), values.end()), 4);

    for (auto& t : receivers) {
        t.join();
    }
}

// 测试批量操作正确性
TEST(MPMCBounded, BatchOperationsCorrectness) {
    BoundedChannel<int> ch(1024);

    // 批量发送
    std::vector<int> sent(1000);
    std::iota(sent.begin(), sent.end(), 0);
    EXPECT_EQ(ch.trySendBatch(sent.begin(), sent.end()), 1000);

    // 批量接收
    std::vector<int> received(1000);
    EXPECT_EQ(ch.tryRecvBatch(received.begin(), 1000), 1000);

    // 验证顺序
    EXPECT_EQ(sent, received);
}
```

### 性能基准

**批量性能测试:**
```bash
# 批量大小扫描
for batch_size in 1 10 50 100 500 1000; do
    ./benchmark_mpmc_batch \
        --topology=4p4c \
        --batch-size=$batch_size \
        --iterations=100000
done
```

**预期结果:**
```
Batch Size | Throughput | vs Phase 1
-----------|------------|------------
1          | 120M/s     | baseline
10         | 160M/s     | +33%
50         | 200M/s     | +67%
100        | 240M/s     | +100%
500        | 300M/s     | +150%
1000       | 320M/s     | +167%
```

### 验收标准

**Phase 2 必须达到:**
- ✅ 相比 Phase 1 提升 ≥15%
- ✅ 批量操作提升 ≥40%
- ✅ 所有正确性测试通过
- ✅ TSan 零告警
- ✅ 公平性测试通过

**期望达到:**
- ✅ 相比 Phase 1 提升 20-25%
- ✅ 批量 100 提升 80-100%
- ✅ 压力测试 72 小时无故障

## Phase 2 时间表

| 周 | 任务 | 负责人 | 状态 |
|----|------|--------|------|
| W4 | 设计侵入式 waiter 栈 | TBD | 待开始 |
| W4 | 实现直接唤醒模型 | TBD | 待开始 |
| W5 | 移除 pump 系统 | TBD | 待开始 |
| W5 | 实现批量操作 | TBD | 待开始 |
| W6 | 无界通道包装层优化 | TBD | 待开始 |
| W6 | 公平性优化 | TBD | 待开始 |
| W7 | 完整测试和验证 | TBD | 待开始 |
