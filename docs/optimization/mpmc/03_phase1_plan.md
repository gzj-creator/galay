# MPMC Phase 1: 热路径优化 (2-3 周)

## 优先级: P0 - 关键

**目标:** 降低内存屏障开销，优化快速路径，移除不必要的同步检查

**预期收益:**
- 有界通道: +20-30% 吞吐
- 无界通道: +15-25% 吞吐

## 1.1 有界通道内存序优化

### 优化点 1: 降低 sequence 发布的内存序

**文件:** `src/cpp/galay-kernel/concurrency/mpmc/bounded_channel.h`

**当前代码 (行 850):**
```cpp
slot->sequence.store(position + 1, std::memory_order_seq_cst);
```

**优化为:**
```cpp
slot->sequence.store(position + 1, std::memory_order_release);
```

**原理:**
- 生产者的 `Release` 与消费者的 `Acquire` 配对
- 保证消息构造对消费者可见
- 不需要 `SeqCst` 的全局顺序

**同时修改消费者端 (行 ~900):**
```cpp
// 当前
const uint64_t sequence = slot->sequence.load(std::memory_order_seq_cst);

// 优化为
const uint64_t sequence = slot->sequence.load(std::memory_order_acquire);
```

**验证要点:**
- TSan 测试必须通过
- 多生产者多消费者压力测试
- 验证消息顺序正确性

**预期收益:** +15-20% 吞吐 (单最大优化项)

### 优化点 2: 优化 waiter 路径标记检查

**当前代码 (行 464):**
```cpp
// 发送后检查接收等待者
if (m_recvWaiterPathUsed.load(std::memory_order_seq_cst)) {
    requestPump(kRecvWork);
}

// 接收后检查发送等待者
if (m_sendWaiterPathUsed.load(std::memory_order_seq_cst)) {
    requestPump(kSendWork);
}
```

**问题:**
- 每次操作都要 SeqCst 读取
- 大多数情况没有等待者
- 不必要的全局同步

**优化策略 1: Relaxed + Fence (快速路径优先):**

```cpp
// 快速路径：relaxed 读取
if (m_recvWaiterPathUsed.load(std::memory_order_relaxed)) {
    // 慢路径：插入屏障后重新检查
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (m_recvWaiterPathUsed.load(std::memory_order_relaxed)) {
        requestPump(kRecvWork);
    }
}
```

**原理:**
- 无等待者时 relaxed 读取足够快
- 检测到等待者后才执行屏障
- 双重检查避免虚假唤醒

**优化策略 2: 批量检查 (适合批量操作):**

```cpp
// 批量发送/接收后只检查一次
template <typename Iterator>
size_t trySendBatch(Iterator begin, Iterator end) {
    size_t sent = 0;
    for (auto it = begin; it != end; ++it) {
        if (trySend(*it)) {
            ++sent;
        } else {
            break;
        }
    }

    // 批次结束后统一检查
    if (sent > 0 &&
        m_recvWaiterPathUsed.load(std::memory_order_relaxed)) {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        if (m_recvWaiterPathUsed.load(std::memory_order_relaxed)) {
            requestPump(kRecvWork);
        }
    }

    return sent;
}
```

**预期收益:**
- 无 waiter 场景: +8-12% 吞吐
- 批量操作: +20-30% 吞吐

### 优化点 3: 改进 CAS 竞争退避策略

**当前代码 (行 88-105):**
```cpp
class BoundedChannelBackoff {
    void snooze() noexcept {
        if (m_step <= kSpinLimit) {
            // 6 步以内：spin
            for (uint32_t i = 0; i < spins; ++i) {
                boundedChannelCpuPause();
            }
        } else {
            // 超过 6 步：yield
            std::this_thread::yield();
        }
        ++m_step;
    }
};
```

**问题:**
- `kSpinLimit = 6` 可能过小
- 高竞争时过早 yield
- yield 开销 ~1000+ cycles

**优化策略:**

```cpp
class BoundedChannelBackoff {
    static constexpr uint32_t kSpinLimit = 10;        // 6 → 10
    static constexpr uint32_t kYieldThreshold = 20;   // 新增 yield 门槛

    void snooze() noexcept {
        if (m_step <= kSpinLimit) {
            // 阶段 1: CPU pause
            const uint32_t spins = 1U << std::min(m_step, 10U);
            for (uint32_t i = 0; i < spins; ++i) {
                boundedChannelCpuPause();
            }
        } else if (m_step <= kYieldThreshold) {
            // 阶段 2: 更多 pause
            for (uint32_t i = 0; i < 1024; ++i) {
                boundedChannelCpuPause();
            }
        } else {
            // 阶段 3: yield
            std::this_thread::yield();
        }
        ++m_step;
    }
};
```

**调优参数 (根据基准测试调整):**
- `kSpinLimit`: 10-12
- `kYieldThreshold`: 15-25
- Pause 重复次数: 512-2048

**预期收益:**
- 2P2C: +5-10%
- 4P4C: +10-15%
- 8P8C: +15-25%

## 1.2 无界通道快速路径优化

### 优化点 1: 减少 waiter 检查开销

**文件:** `src/cpp/galay-kernel/concurrency/mpmc/unbounded_channel.h`

**当前代码 (行 ~900):**
```cpp
bool trySend(T&& value) {
    bool success = m_queue.enqueue(std::move(value));
    if (!success) return false;

    // 每次都检查
    if (m_recvWaiterState.load(std::memory_order_seq_cst) & kRecvWaiterPathUsed) {
        requestPump(kRecvWork);
    }

    return true;
}
```

**优化为:**
```cpp
bool trySend(T&& value) {
    bool success = m_queue.enqueue(std::move(value));
    if (!success) return false;

    // Relaxed 快速路径
    const uint8_t state = m_recvWaiterState.load(std::memory_order_relaxed);
    if (state & kRecvWaiterPathUsed) {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        const uint8_t recheck = m_recvWaiterState.load(std::memory_order_relaxed);
        if (recheck & kRecvWaiterPathUsed) {
            requestPump(kRecvWork);
        }
    }

    return true;
}
```

**预期收益:** +8-12% 吞吐

### 优化点 2: 优化 Token 验证

**当前代码 (行 ~1100):**
```cpp
bool send(ProducerToken& token, T&& value) {
    // 验证 token
    if (!token.validFor(*this)) {
        return false;
    }

    // 检查关闭
    if (isClosed()) {
        return false;
    }

    return sendTokenFast<false>(token, std::move(value));
}
```

**优化策略: 合并检查**

```cpp
bool send(ProducerToken& token, T&& value) {
    // 快速路径：单次检查
    if (token.m_channel != this ||
        token.m_generation != m_generation ||
        m_closeState.load(std::memory_order_relaxed) != 0) {
        return false;
    }

    return sendTokenFast<false>(token, std::move(value));
}
```

**原理:**
- 合并多个分支检查
- 减少函数调用
- 利用 CPU 分支预测

**预期收益:** +3-5% 吞吐 (Token 路径)

### 优化点 3: 批量操作摊销

**新增批量 API:**

```cpp
// 批量发送
template <typename Iterator>
size_t trySendBatch(Iterator begin, Iterator end) {
    // 预先检查关闭
    if (isClosed()) {
        return 0;
    }

    size_t sent = 0;
    for (auto it = begin; it != end; ++it) {
        // 直接调用 moodycamel
        if (!m_queue.enqueue(std::move(*it))) {
            break;
        }
        ++sent;
    }

    // 批次结束统一检查 waiter (只一次)
    if (sent > 0) {
        const uint8_t state = m_recvWaiterState.load(std::memory_order_relaxed);
        if (state & kRecvWaiterPathUsed) {
            std::atomic_thread_fence(std::memory_order_seq_cst);
            if (m_recvWaiterState.load(std::memory_order_relaxed) & kRecvWaiterPathUsed) {
                requestPump(kRecvWork);
            }
        }
    }

    return sent;
}

// 批量接收
template <typename OutputIterator>
size_t tryRecvBatch(OutputIterator dest, size_t maxCount) {
    size_t received = 0;
    T value;

    while (received < maxCount && m_queue.try_dequeue(value)) {
        *dest++ = std::move(value);
        ++received;
    }

    // 批次结束检查发送等待者
    if (received > 0) {
        // ... 类似逻辑
    }

    return received;
}
```

**预期收益:**
- 批量 100: +30-40% 吞吐
- 批量 1000: +50-70% 吞吐

## 1.3 共同优化

### 优化点 1: 精简 requestPump 逻辑

**当前问题:**
- `requestPump` 有 CAS 竞争
- 可能多次调用

**优化策略:**

```cpp
void requestPump(uint8_t work) noexcept {
    const uint8_t current = m_pumpState.load(std::memory_order_relaxed);

    // 快速路径：已经有 pump 在运行
    if ((current & work) != 0) {
        return;
    }

    // 尝试设置标志
    uint8_t expected = current;
    if (m_pumpState.compare_exchange_weak(
            expected, current | work,
            std::memory_order_release,
            std::memory_order_relaxed)) {
        // 成功设置，触发 pump
        runPump(work);
    }
    // 失败表示其他线程已触发，无需重试
}
```

**原理:**
- 避免多次 CAS 尝试
- 失败时快速返回
- 减少 pump 竞争

**预期收益:** +3-5% 吞吐

### 优化点 2: Pump 延迟触发

**策略: 批量累积后触发**

```cpp
class PumpTrigger {
    std::atomic<uint32_t> m_pendingCount{0};
    static constexpr uint32_t kPumpThreshold = 8;  // 累积 8 条消息

    void maybeRequestPump(BoundedChannel* channel, uint8_t work) {
        const uint32_t count = m_pendingCount.fetch_add(1, std::memory_order_relaxed);

        if ((count % kPumpThreshold) == 0) {
            channel->requestPump(work);
        }
    }
};

// 在发送/接收时使用
if (m_recvWaiterPathUsed.load(std::memory_order_relaxed)) {
    m_pumpTrigger.maybeRequestPump(this, kRecvWork);
}
```

**原理:**
- 避免每条消息都触发 pump
- 批量累积减少 pump 开销
- 略增延迟但提升吞吐

**权衡:**
- 吞吐优先场景: 使用延迟触发
- 延迟敏感场景: 立即触发

**预期收益:** +5-10% 吞吐 (高负载)

## 1.4 测试和验证

### 基准测试

**测试矩阵:**
```bash
# 有界通道
for topology in "2p2c" "4p4c" "8p8c"; do
    for capacity in 256 1024 4096; do
        ./benchmark_mpmc_bounded \
            --topology=$topology \
            --capacity=$capacity \
            --messages=10000000
    done
done

# 无界通道
for topology in "2p2c" "4p4c" "8p8c"; do
    ./benchmark_mpmc_unbounded \
        --topology=$topology \
        --messages=10000000
done
```

**对比基准:**
```bash
# 运行 Rust crossbeam 基准
cd benchmark/cpp/kernel/compare/rust-channel
cargo build --release
./target/release/mpmc_paired --case=bounded --topology=2p2c --capacity=4096

# 对比脚本
python3 ../run_mpmc_paired.py \
    --cpp-binary ../../build/mpmc_paired \
    --rust-binary ./target/release/mpmc_paired \
    --output-dir ./results/phase1
```

### 正确性测试

**ThreadSanitizer:**
```bash
cmake -DCMAKE_CXX_FLAGS="-fsanitize=thread -g -O2" ..
make

# 运行所有 MPMC 测试
./test/kernel/t159_mpmc_timeout_race --gtest_repeat=10000
./test/kernel/t162_mpmc_bounded_timeout_race --gtest_repeat=10000
./test/kernel/t167_mpmc_unbounded_block_reuse --gtest_repeat=10000
```

**压力测试 (24 小时):**
```bash
./test/stress/mpmc_stress_test \
    --duration=86400 \
    --topology=4p4c \
    --capacity=1024 \
    --message-rate=1000000
```

**正确性验证点:**
- ✅ 消息不丢失
- ✅ 消息不重复
- ✅ FIFO 顺序保证
- ✅ 无 data race (TSan)
- ✅ 无内存泄漏 (ASan)
- ✅ 无死锁

### 性能验收标准

**Phase 1 必须达到:**
- ✅ 有界 2P2C: +15% 以上
- ✅ 有界 4P4C: +20% 以上
- ✅ 无界 2P2C: +10% 以上
- ✅ 无界 4P4C: +15% 以上
- ✅ 所有正确性测试通过
- ✅ TSan 零告警

**期望达到:**
- ✅ 有界通道整体: +20-30%
- ✅ 无界通道整体: +15-25%
- ✅ 批量操作: +30-50%

## Phase 1 时间表

| 周 | 任务 | 负责人 | 状态 |
|----|------|--------|------|
| W1 | 有界通道内存序优化 | TBD | 待开始 |
| W1 | Waiter 检查优化 | TBD | 待开始 |
| W1 | 单元测试验证 | TBD | 待开始 |
| W2 | 无界通道快速路径 | TBD | 待开始 |
| W2 | CAS 退避策略调优 | TBD | 待开始 |
| W2 | 批量操作实现 | TBD | 待开始 |
| W3 | 完整基准测试 | TBD | 待开始 |
| W3 | 压力测试 (24h) | TBD | 待开始 |
| W3 | 文档更新 | TBD | 待开始 |

## 关键代码变更清单

### 有界通道

**文件:** `src/cpp/galay-kernel/concurrency/mpmc/bounded_channel.h`

1. **行 850:** `seq_cst` → `release`
2. **行 900:** `seq_cst` → `acquire`
3. **行 464:** 添加 relaxed + fence 优化
4. **行 88-105:** 调整退避参数
5. **新增:** `trySendBatch()` 方法
6. **新增:** `tryRecvBatch()` 方法

### 无界通道

**文件:** `src/cpp/galay-kernel/concurrency/mpmc/unbounded_channel.h`

1. **行 900:** 添加 relaxed + fence 优化
2. **行 1100:** 合并 token 验证
3. **新增:** `trySendBatch()` 方法
4. **新增:** `tryRecvBatch()` 方法

### 估计代码量

- 修改: ~200 行
- 新增: ~300 行
- 删除: ~50 行
- 净增: ~450 行
