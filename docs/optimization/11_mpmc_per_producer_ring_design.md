# MPMC Per-Producer Ring 设计方案

## 目标

设计并实现一个高性能 MPMC 通道，目标是**全面稳定超过 Rust crossbeam 的 MPMC 性能**。

## 核心设计：Per-Producer Ring

### 架构概述

```
Producer 1 → Ring Buffer 1 ──┐
Producer 2 → Ring Buffer 2 ──┼──→ Consumer 1 (轮询所有 rings)
Producer 3 → Ring Buffer 3 ──┼──→ Consumer 2 (轮询所有 rings)
Producer N → Ring Buffer N ──┘
```

### 关键特性

1. **每个 producer 独占一个 ring buffer**
   - 消除 producer 之间的竞争
   - 每个 producer 写入自己的 ring，无需 CAS 竞争
   - 类似 MPSC 的 per-producer queue，但更优化

2. **Consumer 遍历所有 rings**
   - 每个 consumer 轮询所有 producer rings
   - 从每个 ring 最多取 N 条消息（可配置）
   - N = -1 表示取到空为止（最大吞吐）

3. **配置参数**
   - `max_per_ring_batch`: 单次从一个 ring 最多取多少条
     - `-1`: 取到空为止（吞吐优先）
     - `1`: 严格轮询（公平性优先）
     - `N > 1`: 平衡吞吐与公平性

## 与现有实现对比

### 当前 Bounded MPMC (Vyukov-style)
- **优点**: 
  - 容量固定，内存可预测
  - 实现成熟，经过充分测试
- **缺点**:
  - 所有 producer 竞争同一个 tail cursor (CAS)
  - 高并发下竞争激烈，性能瓶颈

### 当前 Unbounded MPMC (分段 + token)
- **优点**:
  - Token 优化减少查找开销
  - 分段设计支持无界增长
- **缺点**:
  - 仍有 tail cursor 竞争
  - 分段管理有额外开销

### Per-Producer Ring (新设计)
- **优点**:
  - **零 producer 竞争**: 每个 producer 独占 ring
  - **缓存友好**: 每个 producer 的数据局部性好
  - **可扩展**: 添加 producer 不影响现有性能
- **潜在挑战**:
  - Consumer 需要轮询多个 rings
  - 需要设计高效的轮询策略

## 设计细节

### 1. Producer Ring Buffer

```cpp
template <typename T, size_t Capacity>
class ProducerRing {
private:
    struct alignas(64) Slot {
        std::atomic<uint64_t> sequence{0};
        alignas(T) std::byte storage[sizeof(T)];
    };
    
    alignas(128) std::atomic<uint64_t> m_head{0};  // consumer 读位置
    alignas(128) std::atomic<uint64_t> m_tail{0};  // producer 写位置
    std::array<Slot, Capacity> m_slots;
    
public:
    // Producer 专用，无竞争写入
    bool trySend(T&& value);
    
    // Consumer 调用，读取最多 maxCount 条
    size_t tryRecvBatch(T* output, size_t maxCount);
};
```

#### 关键优化点：

1. **Producer 写入 (无竞争)**:
   ```cpp
   bool trySend(T&& value) {
       uint64_t tail = m_tail.load(relaxed);  // 只有自己写，relaxed 足够
       uint64_t head = m_head.load(acquire);  // 需要看到 consumer 的释放
       
       if (tail - head >= Capacity) {
           return false;  // 满了
       }
       
       Slot& slot = m_slots[tail & mask];
       construct_at(slot.value(), std::move(value));
       slot.sequence.store(tail + 1, release);  // 发布给 consumer
       m_tail.store(tail + 1, relaxed);  // 只有自己看，relaxed
       return true;
   }
   ```

2. **Consumer 批量读取**:
   ```cpp
   size_t tryRecvBatch(T* output, size_t maxCount) {
       uint64_t head = m_head.load(relaxed);
       size_t received = 0;
       
       while (received < maxCount) {
           Slot& slot = m_slots[head & mask];
           uint64_t seq = slot.sequence.load(acquire);
           
           if (seq != head + 1) {
               break;  // 没有更多数据
           }
           
           output[received] = std::move(*slot.value());
           destroy_at(slot.value());
           slot.sequence.store(head + Capacity, release);  // 释放给 producer
           ++head;
           ++received;
       }
       
       if (received > 0) {
           m_head.store(head, release);  // 发布给 producer
       }
       return received;
   }
   ```

### 2. MPMC Channel

```cpp
template <typename T>
class PerProducerMPMC {
private:
    struct ProducerState {
        std::unique_ptr<ProducerRing<T, CAPACITY>> ring;
        std::atomic<bool> active{true};
        uint64_t producerId;
    };
    
    std::vector<ProducerState> m_producers;
    std::atomic<size_t> m_producerCount{0};
    std::atomic<bool> m_closed{false};
    
    // 配置参数
    size_t m_maxPerRingBatch;  // -1 表示无限制
    
public:
    // Producer 注册，获取专属 token
    ProducerToken registerProducer();
    
    // Producer 发送（通过 token）
    bool send(ProducerToken& token, T&& value);
    
    // Consumer 接收
    std::optional<T> tryRecv();
    std::optional<std::vector<T>> tryRecvBatch(size_t count);
};
```

### 3. Consumer 轮询策略

#### 策略 A: 轮询公平性（maxPerRingBatch = 1）
```cpp
std::optional<T> tryRecv() {
    size_t producerCount = m_producerCount.load(acquire);
    
    for (size_t i = 0; i < producerCount; ++i) {
        size_t idx = (m_lastProducerIdx + i) % producerCount;
        
        T value;
        if (m_producers[idx].ring->tryRecvBatch(&value, 1) > 0) {
            m_lastProducerIdx = (idx + 1) % producerCount;
            return value;
        }
    }
    
    return std::nullopt;
}
```

#### 策略 B: 吞吐优先（maxPerRingBatch = -1）
```cpp
std::optional<std::vector<T>> tryRecvBatch(size_t count) {
    std::vector<T> results;
    results.reserve(count);
    
    size_t producerCount = m_producerCount.load(acquire);
    
    while (results.size() < count) {
        bool foundAny = false;
        
        for (size_t i = 0; i < producerCount && results.size() < count; ++i) {
            size_t idx = (m_lastProducerIdx + i) % producerCount;
            
            size_t remaining = count - results.size();
            size_t batchSize = m_maxPerRingBatch == -1 
                ? remaining 
                : std::min(remaining, m_maxPerRingBatch);
            
            T batch[batchSize];
            size_t received = m_producers[idx].ring->tryRecvBatch(batch, batchSize);
            
            if (received > 0) {
                results.insert(results.end(), 
                              std::make_move_iterator(batch),
                              std::make_move_iterator(batch + received));
                foundAny = true;
            }
        }
        
        if (!foundAny) break;
    }
    
    return results.empty() ? std::nullopt : std::make_optional(results);
}
```

#### 策略 C: 平衡模式（maxPerRingBatch = N）
- N 典型值: 8, 16, 32, 64
- 每次从一个 ring 最多取 N 条
- 避免单个 ring 霸占 consumer 时间
- 保证一定的公平性

### 4. 容量设计

#### Bounded 版本
- 每个 producer ring 容量固定（如 4096）
- 总容量 = `producerCount * ringCapacity`
- 内存可预测，适合实时系统

#### Unbounded 版本
- 每个 producer ring 可动态扩展
- 使用分段链表，类似现有 unbounded 实现
- 低负载时节省内存

## 性能预期

### 理论分析

1. **Producer 侧**:
   - 无 CAS 竞争 → 接近 SPSC 性能
   - 缓存行独占 → 无 false sharing
   - 预期吞吐: **200M+ msg/s per producer**

2. **Consumer 侧**:
   - 轮询开销: O(P) 其中 P = producer 数量
   - 批量读取减少内存屏障
   - 预期: **单 consumer 100M+ msg/s**

3. **vs Crossbeam**:
   - Crossbeam bounded: ~80M msg/s (4P4C)
   - 目标: **120M+ msg/s (4P4C)**
   - 提升: **50%+**

### 关键性能因素

| 因素 | Crossbeam | Per-Producer Ring | 提升 |
|------|-----------|-------------------|------|
| Producer CAS 竞争 | 高 | 无 | ✓✓✓ |
| Consumer CAS 竞争 | 高 | 中 | ✓ |
| 缓存行竞争 | 高 | 低 | ✓✓ |
| 批量操作 | 有限 | 优化 | ✓ |

## 实现计划

### Phase 1: 基础框架（2-3 天）
- [ ] `ProducerRing<T, Capacity>` 实现
- [ ] `PerProducerBoundedMPMC<T>` 框架
- [ ] Producer token 注册机制
- [ ] 基本单元测试

### Phase 2: 轮询策略（2 天）
- [ ] 轮询公平性策略
- [ ] 吞吐优先策略
- [ ] 平衡策略（可配置 N）
- [ ] 策略性能测试

### Phase 3: 性能优化（3-4 天）
- [ ] 缓存行对齐优化
- [ ] Prefetch 优化
- [ ] Consumer 本地缓存
- [ ] 批量操作优化

### Phase 4: Benchmark 对比（2-3 天）
- [ ] 实现 crossbeam 兼容的 benchmark
- [ ] 2P2C, 4P4C, 8P8C 测试
- [ ] 不同 batch size 测试
- [ ] 不同容量测试
- [ ] 生成性能报告

### Phase 5: Unbounded 版本（3-4 天）
- [ ] 分段扩展设计
- [ ] 内存回收策略
- [ ] Unbounded benchmark

## Benchmark 测试矩阵

### 配置组合
```
Topology: 2P2C, 4P4C, 8P8C
Capacity: 1024, 4096, 16384
MaxPerRingBatch: 1, 8, 16, 32, 64, -1
MessageCount: 1M, 5M, 10M, 100M
```

### 关键指标
- **吞吐量**: messages/second
- **延迟**: p50, p99, p999 (ns)
- **公平性**: 每个 producer/consumer 的分布
- **CPU 使用率**: user/system time
- **缓存效率**: L1/L2/L3 miss rate

### 对比基准
1. Crossbeam bounded MPMC
2. Crossbeam unbounded MPMC
3. 现有 galay bounded MPMC
4. 现有 galay unbounded MPMC

## 潜在优化

### 1. Consumer 亲和性
每个 consumer 优先轮询特定的 producer rings，减少缓存竞争。

### 2. 动态批量大小
根据负载动态调整 `maxPerRingBatch`：
- 低负载 → 小批量（低延迟）
- 高负载 → 大批量（高吞吐）

### 3. Work Stealing
当某些 consumer 空闲时，可以 steal 其他 consumer 的工作。

### 4. NUMA 优化
Producer ring 分配在 producer 线程的 NUMA 节点上。

### 5. Prefetch 优化
```cpp
// Consumer 预取下一个 ring 的数据
__builtin_prefetch(&m_producers[nextIdx].ring->m_slots[0]);
```

## 风险与缓解

### 风险 1: Consumer 轮询开销
- **缓解**: 批量读取，减少轮询次数
- **缓解**: Consumer 本地缓存，减少实际轮询

### 风险 2: 单个 ring 瓶颈
- **缓解**: 动态批量大小限制
- **缓解**: Work stealing

### 风险 3: Producer 动态注册/注销
- **缓解**: 延迟回收，避免 race
- **缓解**: Epoch-based reclamation

### 风险 4: 内存占用
- **缓解**: Unbounded 版本按需分配
- **缓解**: 可配置 ring capacity

## 成功指标

1. **性能**: 4P4C 下稳定超过 crossbeam 50%+
2. **公平性**: 各 producer/consumer 吞吐方差 < 10%
3. **稳定性**: 1B+ 消息无数据损坏
4. **可用性**: API 简洁，易于使用

## 总结

Per-Producer Ring 设计通过**消除 producer 竞争**，理论上可以达到接近 SPSC 的性能。关键挑战在于优化 consumer 轮询策略，在吞吐量和公平性之间找到平衡。

通过充分的 benchmark 测试和持续优化，有信心实现**全面稳定超过 crossbeam** 的目标。
