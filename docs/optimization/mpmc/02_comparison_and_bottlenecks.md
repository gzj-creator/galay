## 3. 与 Rust Crossbeam 对比

### 3.1 架构差异

**Rust crossbeam 有界通道:**
```rust
// 简化的 crossbeam bounded 结构
pub struct Sender<T> {
    inner: Arc<Channel<T>>,
}

struct Channel<T> {
    // 使用 array-based ring buffer
    buffer: Box<[Slot<T>]>,
    head: AtomicUsize,
    tail: AtomicUsize,
    // 简单的 waker 列表
    receivers: Waker,
    senders: Waker,
}
```

**Galay MPMC 有界通道:**
```cpp
template <BoundedValue T>
class BoundedChannel {
private:
    // Ring buffer
    std::vector<Slot> m_slots;
    std::atomic<uint64_t> m_tail;
    std::atomic<uint64_t> m_head;

    // 复杂的 waiter 管理
    moodycamel::ConcurrentQueue<WaiterPtr> m_recvWaiters;
    moodycamel::ConcurrentQueue<WaiterPtr> m_sendWaiters;
    std::atomic<uint8_t> m_pumpState;
    std::atomic<bool> m_recvWaiterPathUsed;
    std::atomic<bool> m_sendWaiterPathUsed;
};
```

**关键差异:**

| 方面 | Crossbeam | Galay | 影响 |
|------|-----------|-------|------|
| Ring buffer | 简单数组 | std::vector | 相似 |
| Waiter 队列 | 简单链表 | moodycamel 队列 | Galay 更复杂 |
| 唤醒机制 | 直接唤醒 | Pump 系统 | Galay 多层间接 |
| 内存序 | Release/Acquire | 过多 SeqCst | Galay 保守 |
| 状态标记 | 最小化 | 多个原子标记 | Galay 开销大 |

### 3.2 关键路径对比

**发送路径 (无 waiter 场景):**

**Crossbeam:**
```
1. CAS tail (Relaxed/Acquire)
2. 构造消息
3. Store sequence (Release)
4. 可选：检查 receiver waker
---------------------------
总计: ~50-60 cycles
```

**Galay:**
```
1. Load tail (Relaxed)
2. CAS 竞争循环 (Relaxed)
3. 构造消息
4. Store sequence (SeqCst)         ← +15 cycles
5. Load waiterPathUsed (SeqCst)    ← +15 cycles
6. 可选：requestPump
---------------------------
总计: ~80-115 cycles (+40-90%)
```

**接收路径 (无 waiter 场景):**

**Crossbeam:**
```
1. CAS head (Relaxed/Acquire)
2. Load sequence (Acquire)
3. 移动消息
4. Store sequence (Release)
5. 可选：检查 sender waker
---------------------------
总计: ~50-60 cycles
```

**Galay:**
```
1. Load head (Relaxed)
2. CAS 竞争循环 (Relaxed)
3. Load sequence (Acquire)
4. 移动消息
5. Store sequence (SeqCst)         ← +15 cycles
6. Load waiterPathUsed (SeqCst)    ← +15 cycles
7. 可选：requestPump
---------------------------
总计: ~80-115 cycles (+40-90%)
```

### 3.3 无界通道对比

**Crossbeam unbounded:**
```rust
// 使用自定义的 linked-list 实现
pub struct Channel<T> {
    head: AtomicPtr<Block<T>>,
    tail: AtomicPtr<Block<T>>,
    // ...
}
```

**Galay unbounded:**
```cpp
// 依赖 moodycamel
class UnboundedChannel {
private:
    moodycamel::ConcurrentQueue<T> m_queue;
    // + waiter 系统
};
```

**关键差异:**

1. **Crossbeam:** 自主实现，针对 channel 语义优化
2. **Galay:** 依赖通用库，优化空间受限

**性能对比 (推测):**
- Moodycamel 裸性能: 与 crossbeam 相近或略快
- Galay 包装开销: +15-30%
- **总体差距:** -15-30%

### 3.4 性能基准预测

**有界通道 (capacity=4096):**

| 场景 | Crossbeam | Galay (当前) | 差距 | 主要瓶颈 |
|------|-----------|-------------|------|---------|
| 1P1C | 80M msg/s | 55M msg/s | -31% | SeqCst, waiter检查 |
| 2P1C | 120M msg/s | 75M msg/s | -38% | + tail CAS 竞争 |
| 1P2C | 120M msg/s | 75M msg/s | -38% | + head CAS 竞争 |
| 2P2C | 150M msg/s | 90M msg/s | -40% | 双向竞争 |
| 4P4C | 200M msg/s | 100M msg/s | -50% | 竞争加剧 |

**无界通道:**

| 场景 | Crossbeam | Galay (当前) | 差距 | 主要瓶颈 |
|------|-----------|-------------|------|---------|
| 1P1C | 90M msg/s | 60M msg/s | -33% | 包装开销 |
| 2P2C | 160M msg/s | 100M msg/s | -38% | waiter 检查 |
| 4P4C | 240M msg/s | 130M msg/s | -46% | pump 系统 |

## 4. 性能瓶颈汇总

### 4.1 有界通道瓶颈权重

```
SEQ_CST 开销:           35%  ███████
Waiter 路径检查:        20%  ████
Pump 系统:              20%  ████
CAS 竞争退避:           15%  ███
其他开销:               10%  ██
```

**关键热点:**
1. `slot->sequence.store(pos + 1, seq_cst)` - 每条消息
2. `m_recvWaiterPathUsed.load(seq_cst)` - 每次发送
3. `m_sendWaiterPathUsed.load(seq_cst)` - 每次接收
4. `requestPump()` - 有 waiter 时
5. CAS 竞争循环 - 多生产者/消费者

### 4.2 无界通道瓶颈权重

```
Moodycamel 包装:        30%  ██████
Waiter 检查:            25%  █████
Pump 系统:              20%  ████
Token 验证:             15%  ███
其他开销:               10%  ██
```

**关键热点:**
1. `m_queue.enqueue()` - 第三方库调用
2. `m_recvWaiterState.load(seq_cst)` - 每次发送
3. `requestPump()` - 有 waiter 时
4. `token.validFor()` - Token 路径
5. TLS 查找 - 默认路径

### 4.3 优化优先级矩阵

| 优化项 | 有界收益 | 无界收益 | 难度 | 优先级 |
|--------|---------|---------|------|--------|
| 降低 SEQ_CST | 高 | 中 | 低 | P0 |
| 优化 waiter 检查 | 高 | 高 | 低 | P0 |
| 简化 pump 系统 | 中 | 中 | 高 | P1 |
| 改进 CAS 退避 | 中 | 低 | 低 | P1 |
| 替换 moodycamel | 低 | 高 | 极高 | P2 |
| 批量操作优化 | 中 | 中 | 中 | P1 |

### 4.4 优化潜力评估

**Phase 1 (内存序 + waiter 检查优化):**
- 有界通道: +20-30% 性能
- 无界通道: +15-25% 性能
- 时间: 2-3 周

**Phase 2 (简化 pump + 批量优化):**
- 有界通道: 额外 +15-20%
- 无界通道: 额外 +10-15%
- 时间: 3-4 周

**Phase 3 (架构重构):**
- 有界通道: 额外 +10-15%
- 无界通道: 额外 +20-30% (如果替换 moodycamel)
- 时间: 6-8 周

**累积目标:**
- 有界通道: 从 -40% 缩小到 -10-15%
- 无界通道: 从 -38% 缩小到 -10-20%

## 5. 特殊考虑

### 5.1 MPMC 特有挑战

**1. 双向竞争:**
- 生产者在 tail 上竞争
- 消费者在 head 上竞争
- 优化需要同时考虑两端

**2. 公平性问题:**
- MPMC 需要保证公平性
- 不能让某个生产者/消费者饿死
- 优化不能破坏公平性

**3. Waiter 复杂度:**
- 多个接收者都可能等待
- 唤醒策略需要公平
- 比 MPSC 复杂得多

### 5.2 Moodycamel 依赖

**优点:**
- 成熟稳定的库
- 性能已经很好
- 减少实现工作量

**缺点:**
- 优化空间受限
- 无法深度定制
- 增加二进制大小

**替换考虑:**
- 短期: 保留 moodycamel，优化包装层
- 中期: 评估自实现 waiter 队列
- 长期: 考虑完全自实现无界通道

### 5.3 与 MPSC 的协同

**共同优化机会:**
1. 内存序降级策略相同
2. Pump 系统简化策略相同
3. Waiter 管理可以共享代码

**差异点:**
1. MPMC 需要处理多消费者竞争
2. MPMC 的 waiter 唤醒更复杂
3. MPMC 的公平性要求更高

**建议:**
- 先优化 MPSC，验证策略
- 再应用到 MPMC
- 共享优化后的基础设施
