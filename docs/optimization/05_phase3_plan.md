# Phase 3: 架构重构 (4-6 周)

## 优先级: P2 - 可选

**目标:** 引入专用快速路径，与异步路径分离

## 3.1 双路径架构

### 设计理念

**问题:** 当前架构让同步路径承担异步机制的全部开销

**解决方案:** 分离快速同步路径和慢速异步路径

```
┌─────────────────────────────────────┐
│         BoundedChannel              │
├─────────────────────────────────────┤
│  Fast Path (同步)                    │
│  - trySend()                        │
│  - tryRecv()                        │
│  - 最小原子操作                      │
│  - 无 waiter 开销                   │
├─────────────────────────────────────┤
│  Slow Path (异步)                    │
│  - send() awaitable                 │
│  - recv() awaitable                 │
│  - Timeout 支持                     │
│  - 完整 waiter 机制                 │
└─────────────────────────────────────┘
```

### 3.1.1 快速路径设计

**核心思想:** 完全独立的 ring buffer，零 waiter 开销

```cpp
template <typename T>
class BoundedChannelFastPath {
public:
    explicit BoundedChannelFastPath(size_t capacity)
        : m_capacity(capacity)
        , m_mask(capacity - 1)
        , m_slots(capacity) {
        for (size_t i = 0; i < capacity; ++i) {
            m_slots[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    // 快速发送 - 纯 CAS 无额外检查
    bool trySend(T&& value) noexcept {
        size_t tail = m_tail.load(std::memory_order_relaxed);

        for (;;) {
            Slot& slot = m_slots[tail & m_mask];
            const size_t seq = slot.sequence.load(std::memory_order_acquire);
            const intptr_t diff = static_cast<intptr_t>(seq) -
                                  static_cast<intptr_t>(tail);

            if (diff == 0) {
                // Slot 可用
                if (m_tail.compare_exchange_weak(
                        tail, tail + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
                    // 构造并发布
                    std::construct_at(slot.value(), std::move(value));
                    slot.sequence.store(tail + 1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                // 队列满
                return false;
            } else {
                // Slot 被其他线程占用，重新加载 tail
                tail = m_tail.load(std::memory_order_relaxed);
            }
        }
    }

    // 快速接收 - 纯读取无副作用
    std::optional<T> tryRecv() noexcept {
        size_t head = m_head.load(std::memory_order_relaxed);

        for (;;) {
            Slot& slot = m_slots[head & m_mask];
            const size_t seq = slot.sequence.load(std::memory_order_acquire);
            const intptr_t diff = static_cast<intptr_t>(seq) -
                                  static_cast<intptr_t>(head + 1);

            if (diff == 0) {
                // 消息可用
                if (m_head.compare_exchange_weak(
                        head, head + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
                    T value = std::move(*slot.value());
                    std::destroy_at(slot.value());
                    slot.sequence.store(head + m_capacity,
                                       std::memory_order_release);
                    return value;
                }
            } else if (diff < 0) {
                // 队列空
                return std::nullopt;
            } else {
                head = m_head.load(std::memory_order_relaxed);
            }
        }
    }

private:
    struct Slot {
        alignas(T) std::byte storage[sizeof(T)];
        std::atomic<size_t> sequence{0};

        T* value() noexcept {
            return std::launder(reinterpret_cast<T*>(storage));
        }
    };

    alignas(128) std::atomic<size_t> m_tail{0};
    alignas(128) std::atomic<size_t> m_head{0};
    const size_t m_capacity;
    const size_t m_mask;
    std::vector<Slot> m_slots;
};
```

**关键特性:**
- 零 waiter 检查
- 零 close 检查
- 纯 Release/Acquire 语义
- 与 Dmitry Vyukov MPMC queue 算法一致

### 3.1.2 集成快速和慢速路径

```cpp
template <typename T>
class BoundedChannel {
public:
    // 同步 API - 使用快速路径
    bool trySend(T&& value) noexcept {
        return m_fastPath.trySend(std::move(value));
    }

    std::optional<T> tryRecv() noexcept {
        return m_fastPath.tryRecv();
    }

    // 异步 API - 先尝试快速路径，再回退慢速路径
    BoundedSendAwaitable<T> send(T&& value) {
        // 快速路径优先
        if (m_fastPath.trySend(std::move(value))) {
            return BoundedSendAwaitable<T>::makeReady();
        }

        // 回退到慢速等待
        return BoundedSendAwaitable<T>(this, std::move(value));
    }

    BoundedRecvAwaitable<T> recv() {
        // 快速路径优先
        if (auto value = m_fastPath.tryRecv()) {
            return BoundedRecvAwaitable<T>::makeReady(std::move(*value));
        }

        // 回退到慢速等待
        return BoundedRecvAwaitable<T>(this);
    }

    // Close 只影响慢速路径
    void close() noexcept {
        m_closed.store(true, std::memory_order_release);
        wakeAllWaiters();
    }

private:
    BoundedChannelFastPath<T> m_fastPath;
    std::atomic<bool> m_closed{false};
    WaiterSlot m_sendWaiter;
    WaiterSlot m_recvWaiter;
};
```

**优势:**
- 同步路径达到最优性能
- 异步功能完整保留
- API 向后兼容

**预期收益:**
- 同步 trySend/tryRecv: 接近 Rust crossbeam 性能 (95%+)
- 异步 send/recv: 保持现有功能
- 混合使用: 最佳的两个世界

## 3.2 无界通道零注册路径

### 3.2.1 Per-Thread Stream 优化

**当前问题:**
- Producer 注册需要全局协调
- 关闭时需要等待所有注册

**优化方案:** 基于 epoch 的免注册发送

```cpp
class UnboundedChannel {
public:
    // Epoch-based 关闭协议
    bool send(T&& value) noexcept {
        ProducerStream* stream = defaultProducerStream();
        if (stream == nullptr) {
            return false;
        }

        // 进入发送 epoch
        const uint64_t epoch = m_epoch.load(std::memory_order_acquire);
        if (epoch & kClosingBit) {
            return false;
        }

        // 无注册发送
        bool success = sendToStreamDirect(*stream, std::move(value), epoch);

        return success;
    }

    bool close() noexcept {
        // 设置 closing bit
        uint64_t epoch = m_epoch.fetch_or(kClosingBit,
                                          std::memory_order_acq_rel);
        if (epoch & kClosingBit) {
            return false;  // 已关闭
        }

        // 等待当前 epoch 的所有发送完成
        waitForEpoch(epoch);

        // 唤醒等待者
        wakeAllWaiters();

        return true;
    }

private:
    bool sendToStreamDirect(ProducerStream& stream, T&& value,
                           uint64_t entryEpoch) noexcept {
        // 构造消息
        if (!reserveAndConstruct(stream, std::move(value))) {
            return false;
        }

        // 发布前再次检查 epoch
        const uint64_t currentEpoch = m_epoch.load(std::memory_order_acquire);
        if (currentEpoch != entryEpoch) {
            // 关闭发生在构造期间，回滚
            rollbackLastMessage(stream);
            return false;
        }

        // 发布
        publishStream(stream);
        return true;
    }

    void waitForEpoch(uint64_t epoch) noexcept {
        // 等待所有 stream 的 published 计数稳定
        ProducerStream* stream = m_streamHead.load(std::memory_order_acquire);
        while (stream != nullptr) {
            uint64_t published = stream->shared.published.load(
                std::memory_order_acquire);

            // 自旋等待没有 in-flight 的发送
            while (stream->producer.localPublished > published) {
                std::this_thread::yield();
                published = stream->shared.published.load(
                    std::memory_order_acquire);
            }

            stream = stream->next;
        }
    }

    alignas(128) std::atomic<uint64_t> m_epoch{0};
    static constexpr uint64_t kClosingBit = 1ULL << 63;
};
```

**优势:**
- 零注册开销
- 关闭协议更简单
- 支持高并发

**预期收益:**
- 多生产者场景: +40-60% 吞吐

## 3.3 SIMD 批量操作

### 3.3.1 批量 ready flag 检查

**当前代码:**
```cpp
// 逐个检查 ready flag
for (size_t i = 0; i < count; ++i) {
    if (block->ready[i].load(std::memory_order_acquire) == 0) {
        break;
    }
    // 处理消息
}
```

**SIMD 优化:**
```cpp
#include <immintrin.h>  // AVX2

size_t scanReadyBatch(Block* block, size_t maxCount) noexcept {
    size_t count = 0;

#if defined(__AVX2__)
    // 每次检查 32 个 flag
    while (count + 32 <= maxCount) {
        // 加载 32 个 uint8_t flag
        __m256i flags = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(&block->ready[count]));

        // 比较是否全为 1
        __m256i ones = _mm256_set1_epi8(1);
        __m256i cmp = _mm256_cmpeq_epi8(flags, ones);

        // 生成掩码
        uint32_t mask = _mm256_movemask_epi8(cmp);

        if (mask != 0xFFFFFFFF) {
            // 找到第一个 0
            count += __builtin_ctz(~mask);
            return count;
        }

        count += 32;
    }
#endif

    // 处理剩余
    while (count < maxCount) {
        if (block->ready[count].load(std::memory_order_acquire) == 0) {
            break;
        }
        ++count;
    }

    return count;
}
```

**预期收益:**
- 批量接收 1000: +20-30%

### 3.3.2 批量内存拷贝优化

**对于 trivially copyable 类型:**

```cpp
template <typename T>
    requires std::is_trivially_copyable_v<T>
void copyBatch(T* dest, const Slot* src, size_t count) noexcept {
    // 使用 memcpy，编译器会优化为 SIMD
    for (size_t i = 0; i < count; ++i) {
        std::memcpy(dest + i, src[i].value(), sizeof(T));
    }
}
```

## 3.4 测试和验证

### 3.4.1 性能目标

**相对于 Rust crossbeam:**

| 场景 | Phase 2 差距 | Phase 3 目标 |
|------|-------------|-------------|
| 有界 1P1C | -15% | -5% |
| 有界 4P1C | -30% | -10% |
| 有界 8P1C | -40% | -15% |
| 无界 1P1C | -20% | -8% |
| 无界 4P1C | -35% | -12% |
| 无界 8P1C | -50% | -15% |

### 3.4.2 兼容性测试

**确保 API 兼容性:**

```cpp
// 所有现有测试必须通过
TEST(BoundedChannel, BackwardCompatibility) {
    BoundedChannel<int> ch(1024);

    // 同步 API
    EXPECT_TRUE(ch.trySend(42));
    EXPECT_EQ(ch.tryRecv(), 42);

    // 异步 API
    auto sendTask = ch.send(100);
    auto recvTask = ch.recv();

    // Timeout
    auto timedRecv = ch.recv().timeout(100ms);

    // Close
    ch.close();
    EXPECT_TRUE(ch.isClosed());
}
```

### 3.4.3 压力测试

**长时间混合负载:**

```cpp
// 同时使用同步和异步 API
void mixedWorkload(BoundedChannel<int>& ch) {
    std::vector<std::thread> threads;

    // 同步生产者
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([&ch] {
            for (int j = 0; j < 1000000; ++j) {
                while (!ch.trySend(j)) {
                    std::this_thread::yield();
                }
            }
        });
    }

    // 异步消费者
    auto consumer = [&ch]() -> Task<void> {
        for (int i = 0; i < 4000000; ++i) {
            int value = co_await ch.recv().timeout(1s);
            processValue(value);
        }
    };

    runAsync(consumer());

    for (auto& t : threads) {
        t.join();
    }
}
```

**运行时间:** 72 小时连续运行

**监控指标:**
- 内存泄漏
- Race condition
- 死锁
- 性能退化

## Phase 3 时间表

| 周 | 任务 | 负责人 | 状态 |
|----|------|--------|------|
| W8-9 | 快速路径设计和原型 | TBD | 待开始 |
| W9-10 | 集成快速和慢速路径 | TBD | 待开始 |
| W10 | Epoch-based 关闭协议 | TBD | 待开始 |
| W11 | SIMD 批量操作 | TBD | 待开始 |
| W12 | 性能调优 | TBD | 待开始 |
| W13 | 完整测试套件 | TBD | 待开始 |
