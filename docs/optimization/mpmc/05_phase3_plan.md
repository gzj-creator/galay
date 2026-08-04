# MPMC Phase 3: 高级优化 (4-6 周)

## 优先级: P2 - 可选

**目标:** 探索架构级优化，考虑替换底层依赖，实现专用快速路径

**预期收益:**
- 有界通道: 额外 +10-15% 吞吐
- 无界通道: 额外 +15-25% 吞吐

## 3.1 有界通道：双路径架构

### 设计理念

**问题:** 异步 waiter 机制拖累同步快速路径

**解决方案:** 分离快速同步路径和慢速异步路径

```
┌─────────────────────────────────────┐
│      MPMC BoundedChannel            │
├─────────────────────────────────────┤
│  Fast Path (同步)                    │
│  - trySend() / tryRecv()            │
│  - 零 waiter 开销                   │
│  - 最小原子操作                      │
│  - 针对高吞吐优化                    │
├─────────────────────────────────────┤
│  Slow Path (异步)                    │
│  - send() / recv() awaitable        │
│  - Timeout 支持                     │
│  - Waiter 管理                      │
│  - 针对低延迟/阻塞场景               │
└─────────────────────────────────────┘
```

### 实现策略

**快速路径: 纯 Vyukov MPMC Queue**

```cpp
template <typename T>
class FastMPMCRing {
public:
    explicit FastMPMCRing(size_t capacity)
        : m_capacity(std::bit_ceil(capacity))
        , m_mask(m_capacity - 1)
        , m_slots(m_capacity) {
        for (size_t i = 0; i < m_capacity; ++i) {
            m_slots[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    // 快速发送 - 纯算法，零额外检查
    bool trySend(T&& value) noexcept {
        uint64_t tail = m_tail.load(std::memory_order_relaxed);

        for (;;) {
            Slot& slot = m_slots[tail & m_mask];
            const uint64_t seq = slot.sequence.load(std::memory_order_acquire);
            const int64_t diff = static_cast<int64_t>(seq) -
                                 static_cast<int64_t>(tail);

            if (diff == 0) {
                // Slot 可用
                if (m_tail.compare_exchange_weak(
                        tail, tail + 1,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
                    std::construct_at(slot.value(), std::move(value));
                    slot.sequence.store(tail + 1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                // 队列满
                return false;
            } else {
                // 其他线程占用，重新加载
                tail = m_tail.load(std::memory_order_relaxed);
            }
        }
    }

    // 快速接收
    std::optional<T> tryRecv() noexcept {
        uint64_t head = m_head.load(std::memory_order_relaxed);

        for (;;) {
            Slot& slot = m_slots[head & m_mask];
            const uint64_t seq = slot.sequence.load(std::memory_order_acquire);
            const int64_t diff = static_cast<int64_t>(seq) -
                                 static_cast<int64_t>(head + 1);

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
        std::atomic<uint64_t> sequence;

        T* value() noexcept {
            return std::launder(reinterpret_cast<T*>(storage));
        }
    };

    alignas(128) std::atomic<uint64_t> m_tail{0};
    alignas(128) std::atomic<uint64_t> m_head{0};
    const size_t m_capacity;
    const size_t m_mask;
    std::vector<Slot> m_slots;
};
```

**集成快速和慢速路径:**

```cpp
template <typename T>
class BoundedChannel {
public:
    // 同步 API - 使用快速路径
    bool trySend(T&& value) noexcept {
        return m_fastRing.trySend(std::move(value));
    }

    std::optional<T> tryRecv() noexcept {
        return m_fastRing.tryRecv();
    }

    // 异步 API - 先尝试快速路径
    BoundedSendAwaitable<T> send(T&& value) {
        if (m_fastRing.trySend(std::move(value))) {
            return BoundedSendAwaitable<T>::makeReady();
        }

        // 回退到慢速 waiter 路径
        return BoundedSendAwaitable<T>(this, std::move(value));
    }

    BoundedRecvAwaitable<T> recv() {
        if (auto value = m_fastRing.tryRecv()) {
            return BoundedRecvAwaitable<T>::makeReady(std::move(*value));
        }

        // 回退到慢速 waiter 路径
        return BoundedRecvAwaitable<T>(this);
    }

private:
    FastMPMCRing<T> m_fastRing;            // 快速路径
    WaiterStack<T> m_recvWaiters;          // 慢速路径
    WaiterStack<T> m_sendWaiters;
    std::atomic<bool> m_closed{false};
};
```

**优势:**
- 同步路径零 waiter 开销
- 异步路径功能完整
- API 完全兼容

**预期收益:**
- 同步 trySend/tryRecv: 接近 Rust crossbeam (95%+)
- 异步 send/recv: 保持现有功能
- 混合使用: 最佳折衷

## 3.2 无界通道：评估自实现

### Moodycamel 的局限

**当前依赖:**
- 通用 MPMC 队列
- 优化针对通用场景
- 无法针对 channel 语义定制

**自实现的潜在优势:**
1. **针对性优化** - 专门为 channel 设计
2. **减少包装层** - 直接集成 waiter
3. **更小的二进制** - 不包含不需要的功能
4. **完全控制** - 可深度优化

### 设计方案: 分段链表

**架构:**

```cpp
template <typename T>
class CustomUnboundedMPMC {
    // 每个分段 4096 个 slot
    static constexpr size_t kSegmentCapacity = 4096;

    struct Segment {
        std::array<Slot<T>, kSegmentCapacity> slots;
        std::atomic<Segment*> next{nullptr};
        std::atomic<uint64_t> base{0};  // 此分段的起始索引
    };

    struct Slot {
        alignas(T) std::byte storage[sizeof(T)];
        std::atomic<uint8_t> ready{0};  // 0=empty, 1=ready

        T* value() noexcept {
            return std::launder(reinterpret_cast<T*>(storage));
        }
    };

    // 生产者端
    alignas(128) std::atomic<uint64_t> m_tail{0};
    alignas(128) std::atomic<Segment*> m_tailSegment;

    // 消费者端
    alignas(128) std::atomic<uint64_t> m_head{0};
    alignas(128) std::atomic<Segment*> m_headSegment;

public:
    bool trySend(T&& value) noexcept {
        // 1. CAS 获取 tail 位置
        const uint64_t tail = m_tail.fetch_add(1, std::memory_order_relaxed);

        // 2. 定位 segment 和 slot
        Segment* segment = findOrAllocSegment(tail);
        const size_t index = tail % kSegmentCapacity;
        Slot& slot = segment->slots[index];

        // 3. 构造消息
        std::construct_at(slot.value(), std::move(value));

        // 4. 发布
        slot.ready.store(1, std::memory_order_release);

        return true;
    }

    std::optional<T> tryRecv() noexcept {
        // 1. 获取 head 位置
        const uint64_t head = m_head.load(std::memory_order_relaxed);

        // 2. 检查是否有消息
        Segment* segment = m_headSegment.load(std::memory_order_acquire);
        const size_t index = head % kSegmentCapacity;
        Slot& slot = segment->slots[index];

        if (slot.ready.load(std::memory_order_acquire) == 0) {
            // 队列空
            return std::nullopt;
        }

        // 3. 尝试 CAS head
        uint64_t expected = head;
        if (!m_head.compare_exchange_strong(
                expected, head + 1,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            // 被其他消费者抢先
            return std::nullopt;
        }

        // 4. 取出消息
        T value = std::move(*slot.value());
        std::destroy_at(slot.value());
        slot.ready.store(0, std::memory_order_release);

        return value;
    }

private:
    Segment* findOrAllocSegment(uint64_t position) noexcept {
        Segment* current = m_tailSegment.load(std::memory_order_acquire);
        const uint64_t targetBase = (position / kSegmentCapacity) * kSegmentCapacity;

        // 快速路径：当前 segment 可用
        if (current->base.load(std::memory_order_relaxed) == targetBase) {
            return current;
        }

        // 慢路径：需要新 segment
        return allocateSegment(targetBase);
    }

    Segment* allocateSegment(uint64_t base) noexcept {
        Segment* newSegment = new Segment();
        newSegment->base.store(base, std::memory_order_relaxed);

        // 链接到链表
        Segment* current = m_tailSegment.load(std::memory_order_relaxed);
        while (!m_tailSegment.compare_exchange_weak(
                current, newSegment,
                std::memory_order_release,
                std::memory_order_relaxed)) {
            // CAS 重试
        }

        if (current != nullptr) {
            current->next.store(newSegment, std::memory_order_release);
        }

        return newSegment;
    }
};
```

**优势:**
- 无第三方依赖
- 专门为 channel 优化
- 可集成 waiter 系统

**挑战:**
- 实现复杂度高
- 需要充分测试
- 可能无法超越 moodycamel

**评估标准:**
- 性能必须 ≥ moodycamel + 15%
- 正确性经过充分验证
- 代码可维护性可接受

**决策点:**
- 如果 Phase 1+2 已达到 -15% 差距，自实现不必要
- 如果差距仍 >20%，考虑自实现
- 优先级低于有界通道优化

## 3.3 SIMD 优化

### 场景: 批量 ready flag 扫描

**应用于无界通道的批量接收:**

```cpp
#include <immintrin.h>  // AVX2

size_t scanReadySlots(Segment* segment, size_t startIndex, size_t maxCount) {
    size_t count = 0;

#if defined(__AVX2__)
    // 每次检查 32 个 flag
    while (count + 32 <= maxCount) {
        // 加载 32 个 uint8_t ready flag
        __m256i flags = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(
                &segment->slots[startIndex + count].ready));

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
        if (segment->slots[startIndex + count].ready.load(
                std::memory_order_acquire) == 0) {
            break;
        }
        ++count;
    }

    return count;
}

// 批量接收使用 SIMD 扫描
template <typename OutputIterator>
size_t tryRecvBatch(OutputIterator dest, size_t maxCount) {
    const uint64_t head = m_head.load(std::memory_order_relaxed);
    Segment* segment = m_headSegment.load(std::memory_order_acquire);
    const size_t startIndex = head % kSegmentCapacity;

    // SIMD 扫描 ready slot 数量
    const size_t ready = scanReadySlots(segment, startIndex, maxCount);

    if (ready == 0) {
        return 0;
    }

    // 尝试 CAS 认领
    uint64_t expected = head;
    if (!m_head.compare_exchange_strong(
            expected, head + ready,
            std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
        return 0;  // 竞争失败
    }

    // 批量移动消息
    for (size_t i = 0; i < ready; ++i) {
        Slot& slot = segment->slots[startIndex + i];
        *dest++ = std::move(*slot.value());
        std::destroy_at(slot.value());
        slot.ready.store(0, std::memory_order_release);
    }

    return ready;
}
```

**预期收益:**
- 批量 1000: +15-25%
- 仅限批量接收场景

### 场景: 批量内存拷贝

**对 trivially copyable 类型:**

```cpp
template <typename T>
    requires std::is_trivially_copyable_v<T>
size_t tryRecvBatchFast(T* dest, size_t maxCount) {
    // ... 前置检查 ...

    // 使用 memcpy，编译器优化为 SIMD
    for (size_t i = 0; i < ready; ++i) {
        std::memcpy(dest + i, slot[i].value(), sizeof(T));
    }

    // ... 后续清理 ...
}
```

**预期收益:** +5-10% (trivially copyable 类型)

## 3.4 Per-Core Ring Buffer (实验性)

### 设计理念

**问题:** 多生产者/消费者在单个 ring 上竞争

**解决方案:** 每个核心一个独立 ring

```
┌─────────────────────────────────────┐
│     BoundedChannel (4 cores)        │
├─────────────────────────────────────┤
│  Core 0 Ring → Consumer轮询          │
│  Core 1 Ring → Consumer轮询          │
│  Core 2 Ring → Consumer轮询          │
│  Core 3 Ring → Consumer轮询          │
└─────────────────────────────────────┘
```

**实现概要:**

```cpp
template <typename T>
class PerCoreRingChannel {
    static constexpr size_t kMaxCores = 64;

    struct CoreRing {
        FastMPMCRing<T> ring;
        alignas(128) char padding[128];  // 避免 false sharing
    };

    std::array<CoreRing, kMaxCores> m_coreRings;
    std::atomic<uint32_t> m_coreCount{0};

public:
    bool trySend(T&& value) {
        // 发送到当前核心的 ring
        const uint32_t coreId = getCurrentCoreId();
        return m_coreRings[coreId].ring.trySend(std::move(value));
    }

    std::optional<T> tryRecv() {
        // 轮询所有 ring (round-robin)
        static thread_local uint32_t startCore = 0;

        for (uint32_t i = 0; i < m_coreCount.load(std::memory_order_relaxed); ++i) {
            const uint32_t coreId = (startCore + i) % m_coreCount;
            if (auto value = m_coreRings[coreId].ring.tryRecv()) {
                startCore = (coreId + 1) % m_coreCount;
                return value;
            }
        }

        return std::nullopt;
    }
};
```

**优势:**
- 生产者零竞争（每个核心独立）
- 消费者轮询开销可控

**挑战:**
- 需要 CPU affinity 支持
- 负载均衡问题
- 更复杂的实现

**适用场景:**
- 固定核心绑定
- 高竞争场景
- 对吞吐极度敏感

**预期收益:**
- 8P8C: +20-40%
- 16P16C: +40-80%

**风险:**
- 可移植性差
- 公平性问题
- 复杂度高

## 3.5 测试和验证

### 性能目标

**Phase 3 目标 (相对 Rust crossbeam):**

| 场景 | Phase 2 差距 | Phase 3 目标 |
|------|-------------|-------------|
| 有界 2P2C | -20% | -10% |
| 有界 4P4C | -25% | -12% |
| 有界 8P8C | -35% | -15% |
| 无界 2P2C | -18% | -10% |
| 无界 4P4C | -22% | -12% |
| 无界 8P8C | -30% | -15% |

### 验收标准

**必须满足:**
- ✅ 相比 Phase 2 提升 ≥10%
- ✅ 与 Rust 差距 <15%
- ✅ 所有测试通过
- ✅ API 向后兼容

**期望达到:**
- ✅ 某些场景超越 Rust
- ✅ 异步功能完整保留
- ✅ 代码可维护性好

## Phase 3 时间表

| 周 | 任务 | 负责人 | 状态 |
|----|------|--------|------|
| W8-9 | 双路径架构设计和原型 | TBD | 待开始 |
| W9-10 | 实现快速 ring 和集成 | TBD | 待开始 |
| W10 | 评估自实现无界通道 | TBD | 待开始 |
| W11 | SIMD 批量优化 | TBD | 待开始 |
| W12 | Per-core ring 实验 | TBD | 待开始 |
| W13 | 完整测试和性能调优 | TBD | 待开始 |

## 决策点

**Phase 3 是否执行取决于:**

1. **Phase 1+2 成果:**
   - 如果已达到 -10-15% 差距 → Phase 3 可选
   - 如果仍 >20% 差距 → Phase 3 必要

2. **资源情况:**
   - 人力是否充足
   - 时间压力
   - 其他优先级任务

3. **用户需求:**
   - 是否有极端性能需求
   - 是否可接受当前性能
   - 是否愿意等待

**建议:**
- 优先完成 MPSC Phase 3
- MPMC Phase 3 作为长期目标
- 双路径架构优先级最高
- 自实现无界通道优先级最低
