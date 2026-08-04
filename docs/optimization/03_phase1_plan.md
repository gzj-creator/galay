# MPSC 性能优化计划

## 优化策略概览

本计划分为三个阶段，逐步缩小与 Rust crossbeam 的性能差距：

- **Phase 1 (短期)**: 降低内存序强度，优化热路径 - 目标缩小 40-50% 差距
- **Phase 2 (中期)**: 简化同步机制，优化 TLS 缓存 - 目标缩小 70-80% 差距
- **Phase 3 (长期)**: 架构重构，引入快速路径 - 目标缩小 85-95% 差距

## Phase 1: 热路径优化 (2-3 周)

### 优先级: P0 - 关键

**目标:** 降低内存屏障开销，优化快速路径原子操作

### 1.1 有界通道内存序优化

**文件:** `src/cpp/galay-kernel/concurrency/mpsc/bounded_channel.h`

#### 优化点 1: 降低 slot sequence 发布的内存序

**当前代码 (行 962):**
```cpp
slot->sequence.store(position + 1, std::memory_order_seq_cst);
```

**优化为:**
```cpp
slot->sequence.store(position + 1, std::memory_order_release);
```

**原理:**
- `Release` 保证之前的写入（消息构造）对后续 `Acquire` 可见
- 消费者的 `sequence.load(acquire)` 已经提供必要的同步
- 不需要 `SeqCst` 的全局顺序保证

**预期收益:** 单生产者 +15-20% 吞吐

#### 优化点 2: 优化等待者计数检查

**当前代码 (行 505):**
```cpp
if (m_recvWaiterCount.load(std::memory_order_seq_cst) != 0) {
    requestPump(kRecvWork);
}
```

**优化为:**
```cpp
// 快速路径: relaxed 读取
if (m_recvWaiterCount.load(std::memory_order_relaxed) != 0) {
    // 慢路径: 插入屏障后重新检查
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (m_recvWaiterCount.load(std::memory_order_relaxed) != 0) {
        requestPump(kRecvWork);
    }
}
```

**原理:**
- 大多数情况没有等待者，relaxed 读取足够
- 只在检测到等待者时才执行屏障
- 双重检查避免虚假唤醒

**预期收益:** 无等待者场景 +8-12% 吞吐

#### 优化点 3: 批量发送优化

**当前问题:**
- `trySend` 每条消息都检查等待者
- 批量发送没有摊销这个开销

**优化方案:**
```cpp
bool trySendBatch(std::vector<T>&& values) {
    if (values.empty()) return true;

    // 一次性预留所有 slot
    if (!reserveSlots(values.size())) {
        return false;
    }

    // 批量构造，无中间检查
    for (auto& value : values) {
        constructInSlot(std::move(value));
    }

    // 批量发布
    publishBatch(values.size());

    // 只在批次结束检查一次等待者
    if (m_recvWaiterCount.load(std::memory_order_relaxed) != 0) {
        requestPump(kRecvWork);
    }

    return true;
}
```

**预期收益:** 批量发送 +30-40% 吞吐

### 1.2 无界通道快速路径

**文件:** `src/cpp/galay-kernel/concurrency/mpsc/unbounded_channel.h`

#### 优化点 1: 移除热路径的 producer 注册

**当前代码 (行 1223-1231):**
```cpp
ProducerStream* acquireProducerStream() noexcept {
    m_producerRegistrations.fetch_add(1, std::memory_order_seq_cst);
    if (m_closeState.load(std::memory_order_seq_cst) != CloseState::kOpen) {
        m_producerRegistrations.fetch_sub(1, std::memory_order_seq_cst);
        return nullptr;
    }
    // ...
}
```

**优化策略:**
```cpp
// 新增快速路径检查
bool isClosing() const noexcept {
    return m_closeState.load(std::memory_order_acquire) != CloseState::kOpen;
}

// send() 快速路径
bool send(T&& value) noexcept {
    // 快速检查，避免注册
    if (isClosing()) {
        return false;
    }

    ProducerStream* stream = defaultProducerStream();
    if (stream == nullptr) {
        return false;
    }

    return sendToStreamFast(*stream, std::move(value));
}

// sendToStreamFast 不执行 producer 注册
bool sendToStreamFast(ProducerStream& stream, T&& value) noexcept {
    // 直接检查关闭，不注册
    if (isClosing()) {
        return false;
    }

    // 原有的 beginSend/publishStream/finishSend 逻辑
    // ...
}
```

**注意事项:**
- `close()` 需要等待所有 in-flight 的 `sendToStreamFast` 完成
- 使用 gate 状态作为屏障

**预期收益:** +25-35% 吞吐

#### 优化点 2: 简化 beginSend 的内存序

**当前代码 (行 1479-1494):**
```cpp
bool beginSend(ProducerStream& stream) noexcept {
    if (m_closeState.load(std::memory_order_relaxed) != CloseState::kOpen) {
        return false;
    }

    stream.control.gate.store(
        ProducerGate::kSending,
        std::memory_order_seq_cst);

    if (m_closeState.load(std::memory_order_seq_cst) == CloseState::kOpen) {
        return true;
    }

    stream.control.gate.store(ProducerGate::kOpen,
                              std::memory_order_release);
    return false;
}
```

**优化为:**
```cpp
bool beginSend(ProducerStream& stream) noexcept {
    // 快速路径: relaxed 检查
    if (m_closeState.load(std::memory_order_relaxed) != CloseState::kOpen) {
        return false;
    }

    // 降级到 release/acquire
    stream.control.gate.store(
        ProducerGate::kSending,
        std::memory_order_release);

    if (m_closeState.load(std::memory_order_acquire) == CloseState::kOpen) {
        return true;
    }

    stream.control.gate.store(ProducerGate::kOpen,
                              std::memory_order_release);
    return false;
}
```

**注意:** 需要调整 `close()` 中的 gate 读取内存序以匹配

**预期收益:** +10-15% 吞吐

#### 优化点 3: TLS 缓存直接访问

**当前代码 (行 1323-1363):**
- 线性搜索链表
- 每次检查 lifetime state

**优化方案:**
```cpp
struct FastTLSCache {
    const UnboundedChannel* channel = nullptr;
    ProducerStream* stream = nullptr;
    uint64_t generation = 0;

    // 快速路径: 单 channel 直接命中
    bool matches(const UnboundedChannel* ch, uint64_t gen) const noexcept {
        return channel == ch && generation == gen;
    }
};

static FastTLSCache& fastCache() noexcept {
    static thread_local FastTLSCache cache;
    return cache;
}

ProducerStream* defaultProducerStream() noexcept {
    // 快速路径: 单 channel 场景
    FastTLSCache& fast = fastCache();
    if (fast.matches(this, m_generation)) {
        // 验证 stream 仍然有效
        if (fast.stream->lifetime->state.load(std::memory_order_relaxed) ==
            ProducerLifetimeState::kOwned) {
            return fast.stream;
        }
    }

    // 慢路径: 回退到原有逻辑
    ProducerStream* stream = findInSlowCache();
    if (stream != nullptr) {
        // 更新快速缓存
        fast.channel = this;
        fast.stream = stream;
        fast.generation = m_generation;
    }

    return stream;
}
```

**预期收益:** 单 channel 场景 +15-20% 吞吐

### 1.3 测试和验证

**基准测试:**
```bash
# 运行优化前基准
./benchmark/cpp/kernel/compare/run_mpsc_paired.py \
    --cpp-binary ./build/mpsc_paired_baseline \
    --rust-binary ./build/rust_mpsc_paired \
    --output-dir ./results/baseline

# 运行优化后基准
./benchmark/cpp/kernel/compare/run_mpsc_paired.py \
    --cpp-binary ./build/mpsc_paired_phase1 \
    --rust-binary ./build/rust_mpsc_paired \
    --output-dir ./results/phase1

# 对比报告
python3 ./benchmark/compare_results.py \
    ./results/baseline \
    ./results/phase1
```

**正确性测试:**
```bash
# 运行完整测试套件
./build/test/kernel/t151_channel_namespaces
./build/test/kernel/t153_spsc_unbounded
./build/test/kernel/t154_mpsc_unbounded_source
./build/test/kernel/t156_mpsc_timeout_race
./build/test/kernel/t159_mpmc_timeout_race
./build/test/kernel/t163_mpsc_redesign
./build/test/kernel/t166_spsc_paired_final_drain_source
./build/test/kernel/t167_mpmc_unbounded_block_reuse

# 压力测试 (24 小时)
./test/stress/mpsc_stress_test --duration=86400
```

**验收标准:**
- ✅ 所有正确性测试通过
- ✅ 有界通道性能提升 20-30%
- ✅ 无界通道性能提升 30-40%
- ✅ 压力测试无 race/crash

### Phase 1 时间表

| 周 | 任务 | 负责人 | 状态 |
|----|------|--------|------|
| W1 | 有界通道内存序优化 | TBD | 待开始 |
| W1 | 单元测试验证 | TBD | 待开始 |
| W2 | 无界通道快速路径 | TBD | 待开始 |
| W2 | 基准测试对比 | TBD | 待开始 |
| W3 | 压力测试 | TBD | 待开始 |
| W3 | 文档更新 | TBD | 待开始 |
