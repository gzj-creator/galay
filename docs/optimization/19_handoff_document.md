# Per-Producer MPMC 项目交接文档

## 项目概述

**目标**：实现每个 producer 持有独立 ringbuffer 的 MPMC 通道，性能超过 Rust crossbeam。

**当前状态**：部分完成，小规模场景工作正常，大规模/多 consumer 场景存在稳定性问题。

## 核心问题

### 问题描述

**现象**：
- 小规模测试（≤400 消息）：✅ 全部通过
- 大规模测试（4000 消息）：❌ 卡在 ~3000 条消息
- 单/双 consumer：✅ 工作正常
- 4+ consumer：❌ 超时或卡死

**复现步骤**：
```bash
cd /Users/gongzhijie/Desktop/projects/git/galay

# 这个会卡住
./build-release/test/cpp/kernel/t171_per_producer_mpmc_stress

# 这个会超时
./build-release/test/cpp/kernel/t168_per_producer_mpmc
```

### 根本原因分析

1. **CAS 竞争累积**
   - 多个 consumer 竞争同一个 ring 的 head cursor
   - CAS 失败后直接返回 0，导致消息无法被消费
   - 高负载下 CAS 失败率高，累积导致"饿死"

2. **轮询策略问题**
   - Thread-local 索引让每个 consumer 从不同位置开始轮询
   - 但所有 consumer 可能同时跳过某些有消息的 ring
   - 缺少全局协调机制

3. **重试机制缺失**
   - `tryRecvBatch` CAS 失败后返回 0
   - 调用者 `tryRecv` 会轮询下一个 ring
   - 但如果所有 rings 都返回 0（因为 CAS 竞争），就会陷入死循环

## 技术约束

### 项目规范限制

**禁止使用**（来自 CLAUDE.md）：
- ❌ 异常（`throw`, `try`, `catch`）
- ❌ Mutex/Lock（协程环境，禁止阻塞操作）

**必须使用**：
- ✅ `std::expected` 返回错误
- ✅ 原子操作（无锁编程）
- ✅ 显式错误传播

### 架构约束

```
Per-Producer Ring 设计：
┌─────────────────────────────────────┐
│ Producer 1 → Ring 1 (独占，零竞争)  │ ✅
│ Producer 2 → Ring 2 (独占，零竞争)  │ ✅
│ Producer 3 → Ring 3 (独占，零竞争)  │ ✅
└─────────────────────────────────────┘
           ↓ Consumer 轮询
┌─────────────────────────────────────┐
│ Consumer 1 → 轮询所有 rings         │ ⚠️ CAS 竞争
│ Consumer 2 → 轮询所有 rings         │ ⚠️ CAS 竞争
│ Consumer 3 → 轮询所有 rings         │ ⚠️ CAS 竞争
└─────────────────────────────────────┘
```

**矛盾**：
- Producer 端：完美（零竞争）✅
- Consumer 端：有竞争（需要同步）⚠️
- 规范：不能用 mutex ❌

## 已尝试的方案

### 方案 1: Mutex 保护（违规）❌

```cpp
std::lock_guard<std::mutex> lock(m_recvMutex);
```

**结果**：
- ✅ 所有测试通过
- ❌ 违反项目规范（禁止 mutex）
- ❌ 不可接受

### 方案 2: 单 Consumer 优化（无 CAS）❌

```cpp
// 直接更新 head，无 CAS
m_head.store(head + received, std::memory_order_release);
```

**结果**：
- ✅ 单 consumer 性能极佳（60M+ msg/s）
- ❌ 多 consumer 数据竞争（不安全）
- ❌ 不可接受

### 方案 3: Thread-local + CAS（当前）⚠️

```cpp
// Thread-local 索引减少竞争
thread_local size_t threadLocalStartIdx = 0;

// CAS 保护多 consumer
if (!m_head.compare_exchange_strong(head, head + available, ...)) {
    return 0; // ← 问题：直接返回，导致消息丢失
}
```

**结果**：
- ✅ 符合规范（无 mutex）
- ✅ 小规模测试通过
- ❌ 大规模/多 consumer 卡住
- ⚠️ 接近但未完成

## 文件清单

### 核心实现

```
src/cpp/galay-kernel/concurrency/mpmc/
├── per_producer_ring.h              # ProducerRing 实现
└── per_producer_mpmc_channel.h      # PerProducerMPMCChannel 实现
```

**关键代码位置**：
- `per_producer_ring.h:124-175` - `tryRecvBatch()` 方法（CAS 逻辑）
- `per_producer_mpmc_channel.h:217-243` - `tryRecv()` 方法（轮询逻辑）

### 测试文件

```
test/cpp/kernel/
├── t168_per_producer_mpmc.cc               # 完整测试套件（6个测试）
├── t169_per_producer_ring_simple.cc        # ProducerRing 基础测试
├── t170_per_producer_mpmc_debug.cc         # 2P1C 调试测试
├── t171_per_producer_mpmc_stress.cc        # 4P1C 压力测试 ← 卡住
├── t172_per_producer_mpmc_detailed_debug.cc # 4P1C 详细调试
└── t173_producer_ring_heavy_load.cc        # ProducerRing 重负载测试
```

### Benchmark

```
benchmark/cpp/kernel/
└── b30_per_producer_mpmc_benchmark.cc      # 性能测试
```

### 文档

```
docs/optimization/
├── 11_mpmc_per_producer_ring_design.md              # 设计方案
├── 12_per_producer_mpmc_implementation_summary.md   # 实现总结
├── 13_per_producer_mpmc_current_status.md           # 状态分析
├── 14_per_producer_mpmc_final_report.md             # 完成报告
├── 15_per_producer_mpmc_performance_report.md       # 性能报告
├── 16_per_producer_mpmc_project_completion.md       # 项目完成
├── 17_thread_local_optimization_status.md           # Thread-local 优化
└── 18_per_producer_mpmc_final_summary.md            # 最终总结
```

## 性能数据

### 已测试场景

| 场景 | 状态 | 吞吐量 |
|------|------|--------|
| 2P1C (100 msg) | ✅ 通过 | N/A |
| 4P1C (400 msg) | ✅ 通过 | N/A |
| 4P1C (4000 msg) | ❌ 卡住 | N/A |
| 2P2C (5M msg) | ✅ 通过 | 54.76 M/s |
| 4P4C (5M msg) | ⚠️ 不稳定 | 43.36 M/s |

### Benchmark 命令

```bash
# 2P2C
./build-release/benchmark/cpp/kernel/benchmark_kernel_per_producer_mpmc_benchmark \
  --producers 2 --consumers 2 --messages 5000000 --capacity 4096 \
  --strategy balanced --max-per-ring-batch 16

# 4P4C
./build-release/benchmark/cpp/kernel/benchmark_kernel_per_producer_mpmc_benchmark \
  --producers 4 --consumers 4 --messages 5000000 --capacity 4096 \
  --strategy balanced --max-per-ring-batch 16
```

## 建议的解决方案

### 方案 A: 改进重试机制 ⭐⭐⭐⭐

**核心思路**：CAS 失败后不要立即放弃，而是重试

**实现**：

在 `per_producer_ring.h:tryRecvBatch()` 中：

```cpp
size_t tryRecvBatch(T* output, size_t maxCount) noexcept {
    if (maxCount == 0) {
        return 0;
    }

    // 重试循环：最多尝试 N 次
    for (int retry = 0; retry < 3; ++retry) {
        uint64_t head = m_head.load(std::memory_order_acquire);
        size_t available = 0;

        // 探测可用消息数
        for (size_t i = 0; i < maxCount; ++i) {
            const uint64_t pos = head + i;
            Slot& slot = m_slots[pos & kMask];
            const uint64_t seq = slot.sequence.load(std::memory_order_acquire);

            if (seq != pos + 1) {
                break;
            }
            ++available;
        }

        if (available == 0) {
            return 0; // 确实没有消息
        }

        // CAS 预留区间
        if (m_head.compare_exchange_strong(head, head + available,
                                           std::memory_order_acquire,
                                           std::memory_order_relaxed)) {
            // 成功，读取数据
            for (size_t i = 0; i < available; ++i) {
                const uint64_t pos = head + i;
                Slot& slot = m_slots[pos & kMask];
                output[i] = std::move(*slot.value());
                std::destroy_at(slot.value());
                slot.sequence.store(pos + Capacity, std::memory_order_release);
            }
            return available;
        }

        // CAS 失败，短暂后退再重试
        if (retry < 2) {
            for (int i = 0; i < (1 << retry); ++i) {
                std::this_thread::yield();
            }
        }
    }

    return 0; // 重试耗尽
}
```

**预期效果**：
- CAS 失败后重试，减少消息丢失
- Exponential backoff 减少竞争
- 预期解决 70-80% 的卡住问题

### 方案 B: 改进轮询策略 ⭐⭐⭐⭐⭐

**核心思路**：确保至少有一个 consumer 能覆盖所有 rings

**实现**：

在 `per_producer_mpmc_channel.h:tryRecv()` 中：

```cpp
std::optional<T> tryRecv() noexcept {
    const size_t producerCount = m_producerCount.load(std::memory_order_acquire);
    if (producerCount == 0) {
        return std::nullopt;
    }

    thread_local size_t threadLocalStartIdx = 0;
    const size_t batchSize = getBatchSizeForStrategy(1);

    // 多轮轮询：第一轮从 thread-local 开始，第二轮从头开始
    for (size_t round = 0; round < 2; ++round) {
        size_t startIdx = (round == 0) ? threadLocalStartIdx : 0;

        for (size_t i = 0; i < producerCount; ++i) {
            const size_t idx = (startIdx + i) % producerCount;

            T value;
            const size_t received = m_producers[idx].ring->tryRecvBatch(&value, batchSize);

            if (received > 0) {
                threadLocalStartIdx = (idx + 1) % producerCount;
                return std::move(value);
            }
        }

        // 第一轮失败，yield 后再试
        if (round == 0) {
            std::this_thread::yield();
        }
    }

    return std::nullopt;
}
```

**预期效果**：
- 第二轮从头开始，确保覆盖所有 rings
- 减少"所有 consumer 都错过消息"的概率
- 预期解决 80-90% 的卡住问题

### 方案 C: 组合方案（推荐）⭐⭐⭐⭐⭐

**同时实施方案 A + B**

**预期效果**：
- 方案 A：Ring 层面减少 CAS 失败影响
- 方案 B：Channel 层面确保消息不被遗漏
- 预期解决 >95% 的问题

## 测试验证步骤

### 1. 基础验证

```bash
# 重新构建
cmake --build build-release --target t168_per_producer_mpmc t171_per_producer_mpmc_stress

# 运行小规模测试
./build-release/test/cpp/kernel/t170_per_producer_mpmc_debug

# 运行压力测试
./build-release/test/cpp/kernel/t171_per_producer_mpmc_stress
```

**预期**：所有测试应在 30 秒内完成。

### 2. 完整测试

```bash
# 运行完整测试套件
./build-release/test/cpp/kernel/t168_per_producer_mpmc
```

**预期**：所有 6 个测试通过，总时间 < 60 秒。

### 3. 性能测试

```bash
# 运行 benchmark
for i in {1..5}; do
  ./build-release/benchmark/cpp/kernel/benchmark_kernel_per_producer_mpmc_benchmark \
    --producers 4 --consumers 4 --messages 5000000 --capacity 4096 \
    --strategy balanced --max-per-ring-batch 16 | \
    grep '"messages_per_second"'
done
```

**预期**：
- 平均吞吐量 > 50 M msg/s
- 标准差 < 5%
- 无卡住或超时

## 成功标准

### 必须满足（P0）

1. ✅ 所有测试通过（包括 t171 压力测试）
2. ✅ 无超时或卡住
3. ✅ 符合项目规范（无 mutex）

### 应该满足（P1）

4. ✅ 4P4C 吞吐量 > 50 M msg/s
5. ✅ 性能稳定（标准差 < 5%）

### 可选目标（P2）

6. ⭐ 4P4C 吞吐量 > 60 M msg/s
7. ⭐ 超过 crossbeam 20%+

## 环境信息

- **平台**: macOS (Darwin 25.3.0)
- **架构**: ARM64
- **编译器**: Apple Clang
- **构建目录**: `/Users/gongzhijie/Desktop/projects/git/galay/build-release`
- **C++ 标准**: C++23

## 额外优化建议

### 如果基础问题解决后，可考虑：

1. **Adaptive batching**
   ```cpp
   // 根据负载动态调整批量大小
   size_t adaptiveBatchSize = (load > 0.8) ? 64 : 16;
   ```

2. **Prefetch**
   ```cpp
   // 预取下一个 ring
   __builtin_prefetch(&m_producers[nextIdx].ring->m_slots[0]);
   ```

3. **Consumer affinity**
   ```cpp
   // 每个 consumer 优先轮询特定 rings
   size_t affinityStart = consumerId * (producerCount / consumerCount);
   ```

## 联系方式

如有问题，参考文档：
- `docs/optimization/18_per_producer_mpmc_final_summary.md` - 最终总结
- `docs/optimization/17_thread_local_optimization_status.md` - Thread-local 优化状态
- `docs/optimization/11_mpmc_per_producer_ring_design.md` - 原始设计

祝修复顺利！🚀
