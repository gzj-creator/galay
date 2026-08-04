# Galay 通道性能优化计划总览

**版本:** v1.0
**日期:** 2026-08-04
**状态:** 待实施

## 文档结构

```
docs/optimization/
├── README.md                           # 本文件
├── mpsc/                               # MPSC 通道优化计划
│   ├── README.md                       # MPSC 总览
│   ├── 01_performance_analysis.md      # 性能分析
│   ├── 02_unbounded_analysis.md        # 无界通道分析
│   ├── 03_phase1_plan.md              # Phase 1 计划
│   ├── 04_phase2_plan.md              # Phase 2 计划
│   ├── 05_phase3_plan.md              # Phase 3 计划
│   └── 06_risks_and_mitigation.md     # 风险管理
└── mpmc/                               # MPMC 通道优化计划
    ├── README.md                       # MPMC 总览
    ├── 01_performance_analysis.md      # 性能分析
    ├── 02_comparison_and_bottlenecks.md # 对比分析
    ├── 03_phase1_plan.md              # Phase 1 计划
    ├── 04_phase2_plan.md              # Phase 2 计划
    └── 05_phase3_plan.md              # Phase 3 计划
```

## 执行摘要

Galay 的 MPSC 和 MPMC 通道实现功能完善，支持异步协程、超时、取消等高级特性，但在同步高吞吐场景下性能不如 Rust crossbeam。

### 核心问题

**共同问题:**
1. **过度的内存屏障** - 大量使用 `seq_cst` 而非 `release/acquire`
2. **Pump 系统复杂** - 引入额外竞争点和延迟
3. **Waiter 检查开销** - 即使无等待者也要检查

**MPSC 特有:**
- Producer 注册协调开销
- TLS 缓存查找效率低

**MPMC 特有:**
- 多消费者竞争
- 依赖 moodycamel 限制优化空间

### 性能差距

MPSC 性能验收只覆盖 `2P1C`、`4P1C`、`8P1C`。`1P1C` 属于 SPSC 场景，不参与 MPSC 性能结论；单生产者路径仅保留 API、关闭和 awaiter 等正确性验证。

| 通道类型 | 场景 | 当前差距 | 目标差距 |
|---------|------|---------|---------|
| MPSC 有界 | 4P1C | -50% | -10% |
| MPSC 无界 | 4P1C | -68% | -12% |
| MPMC 有界 | 2P2C | -40% | -10% |
| MPMC 有界 | 4P4C | -50% | -12% |
| MPMC 无界 | 2P2C | -38% | -10% |
| MPMC 无界 | 4P4C | -46% | -12% |

## 优化策略

### 三阶段路线图

```
Phase 1: 热路径优化 (2-3 周)
├─ 降低内存序强度
├─ 优化 waiter 检查
├─ 改进 CAS 退避
└─ 预期: +20-40% 性能

Phase 2: 架构简化 (3-4 周)
├─ 简化 waiter 系统
├─ 优化 TLS 缓存
├─ 批量操作专用路径
└─ 预期: 额外 +15-30%

Phase 3: 高级优化 (4-6 周) [可选]
├─ 双路径架构
├─ 考虑自实现
├─ SIMD 优化
└─ 预期: 额外 +10-20%
```

## 快速开始

### 1. 阅读优化计划

**MPSC 通道:**
```bash
# 查看总览
cat docs/optimization/mpsc/README.md

# 深入了解问题
cat docs/optimization/mpsc/01_performance_analysis.md

# 查看实施计划
cat docs/optimization/mpsc/03_phase1_plan.md
```

**MPMC 通道:**
```bash
# 查看总览
cat docs/optimization/mpmc/README.md

# 深入了解问题
cat docs/optimization/mpmc/01_performance_analysis.md

# 查看实施计划
cat docs/optimization/mpmc/03_phase1_plan.md
```

### 2. 运行基准测试

**建立基线:**
```bash
# MPSC 基准
cd benchmark/cpp/kernel/compare
python3 run_mpsc_paired.py \
    --cpp-binary ./build/mpsc_paired \
    --rust-binary ./rust-channel/target/release/mpsc_paired \
    --output-dir ./results/baseline-mpsc

# MPMC 基准
python3 run_mpmc_paired.py \
    --cpp-binary ./build/mpmc_paired \
    --rust-binary ./rust-channel/target/release/mpmc_paired \
    --output-dir ./results/baseline-mpmc
```

### 3. 选择优化任务

**查看 GitHub Issues:**
```bash
gh issue list --label "mpsc-optimization,mpmc-optimization"
```

**推荐起点 (Phase 1):**
- MPSC 有界通道内存序优化
- MPMC 有界通道内存序优化
- Waiter 检查 relaxed + fence 优化

## 关键优化技术

### 1. 内存序降级

**问题:**
```cpp
// 当前: 过度保守
slot->sequence.store(position + 1, std::memory_order_seq_cst);
```

**优化:**
```cpp
// 优化: Release/Acquire 足够
slot->sequence.store(position + 1, std::memory_order_release);
```

**收益:** +15-20% 单条消息吞吐

### 2. Waiter 检查优化

**问题:**
```cpp
// 每次都 seq_cst 检查
if (m_recvWaiterCount.load(std::memory_order_seq_cst) != 0) {
    requestPump(kRecvWork);
}
```

**优化:**
```cpp
// 快速路径 relaxed
if (m_recvWaiterCount.load(std::memory_order_relaxed) != 0) {
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (m_recvWaiterCount.load(std::memory_order_relaxed) != 0) {
        requestPump(kRecvWork);
    }
}
```

**收益:** +8-12% 无 waiter 场景

### 3. 批量操作

**策略:**
```cpp
// 单次 CAS 认领多个 slot
size_t trySendBatch(std::span<T> values) {
    // 1. 批量认领
    if (m_tail.compare_exchange_strong(tail, tail + count, ...)) {
        // 2. 批量构造
        // 3. 批量发布
        // 4. 统一检查 waiter
    }
}
```

**收益:** 批量 100 +60-80%

### 4. 简化 Pump 系统

**当前:** Pump + WaiterQueue → 多层间接

**优化:** 直接唤醒 → 减少延迟

**收益:** +10-15% 吞吐，-30% 延迟

## 预期成果

### MPSC 通道

**Phase 1 完成后:**
- 验收拓扑: 2P1C、4P1C、8P1C
- 差距: -50% → -25%

**Phase 1+2 完成后:**
- 验收拓扑: 2P1C、4P1C、8P1C
- 差距: -25% → -12%

**Phase 1+2+3 完成后:**
- 验收拓扑: 2P1C、4P1C、8P1C
- 差距: -12% → -5%

### MPMC 通道

**Phase 1 完成后:**
- 有界 2P2C: 90M → 115M msg/s (+28%)
- 无界 2P2C: 100M → 125M msg/s (+25%)
- 差距: -40% → -23%

**Phase 1+2 完成后:**
- 有界 2P2C: 115M → 138M msg/s (+20%)
- 无界 2P2C: 125M → 143M msg/s (+14%)
- 差距: -23% → -8%

**Phase 1+2+3 完成后:**
- 有界 2P2C: 138M → 154M msg/s (+12%)
- 无界 2P2C: 143M → 162M msg/s (+13%)
- 差距: -8% → +3%

## 实施时间表

### 总体规划

```
2026-08     │ MPSC Phase 1 + MPMC Phase 1
2026-09     │ MPSC Phase 2 + MPMC Phase 2
2026-10-11  │ MPSC Phase 3 + MPMC Phase 3 [可选]
2026-11     │ v2.0 正式发布
```

### 并行策略

**Week 1-3:** MPSC 和 MPMC Phase 1 并行
- 可以不同人负责
- 共享内存序优化经验
- 统一的测试框架

**Week 4-7:** MPSC 和 MPMC Phase 2 并行
- 共享 waiter 系统设计
- 统一批量操作 API
- 协同测试

**Week 8-13:** Phase 3 (可选)
- MPSC 优先
- MPMC 跟进
- 根据 Phase 1+2 成果决定是否执行

## 验收标准

### Phase 1

**必须达到:**
- ✅ MPSC 有界: +20% 以上
- ✅ MPSC 无界: +30% 以上
- ✅ MPMC 有界: +20% 以上
- ✅ MPMC 无界: +15% 以上
- ✅ 所有测试通过
- ✅ TSan 零告警

### Phase 2

**必须达到:**
- ✅ 相比 Phase 1 再提升 15% 以上
- ✅ 批量操作提升 40% 以上
- ✅ 压力测试 72 小时无故障

### Phase 3

**必须达到:**
- ✅ 相比 Phase 2 再提升 10% 以上
- ✅ 与 Rust 差距 <15%
- ✅ API 向后兼容

## 风险管理

### 技术风险

**R1: 内存序降级导致 Race**
- 缓解: TSan, 多平台测试, 压力测试
- 影响: 高
- 概率: 中

**R2: 性能不达预期**
- 缓解: 早期基准, 及时调整
- 影响: 中
- 概率: 低

**R3: 破坏 ABI 兼容性**
- 缓解: 主版本升级, 迁移指南
- 影响: 中
- 概率: 中

### 项目风险

**R4: 时间延期**
- 缓解: 分阶段交付, Phase 3 可选
- 影响: 低
- 概率: 中

**R5: 人力不足**
- 缓解: 并行开发, 外部专家
- 影响: 中
- 概率: 低

## 贡献指南

### 如何参与

1. **阅读文档** - 理解问题和方案
2. **选择任务** - GitHub Issues 标签过滤
3. **开发测试** - 遵循编码规范
4. **提交 PR** - 包含测试和基准

### 代码审查要求

**必须满足:**
- ✅ 通过 TSan/ASan
- ✅ 包含单元测试
- ✅ 性能基准对比
- ✅ 两位 reviewer 批准

### 测试要求

**正确性:**
- 单元测试覆盖 >90%
- TSan 零告警
- 压力测试 24 小时+

**性能:**
- 基准测试对比
- 无回归验证
- 多场景测试

## 参考资源

### 文档

- [MPSC 优化计划](./mpsc/README.md)
- [MPMC 优化计划](./mpmc/README.md)
- [内存模型指南](../guides/memory_model.md)
- [并发测试最佳实践](../guides/concurrent_testing.md)

### 论文

1. **Vyukov MPMC Queue** - 有界 MPMC 算法
2. **Michael & Scott MPSC** - 无界 MPSC 算法
3. **Herlihy & Shavit** - 并发编程教材

### 项目

1. **Rust Crossbeam** - 参考实现
2. **Moodycamel ConcurrentQueue** - 底层依赖
3. **Folly** - Facebook 的实现

## 联系方式

**问题和讨论:**
- GitHub Issues: https://github.com/galay/galay/issues
- 邮件: dev@galay.org
- Slack: #channel-optimization

**紧急问题:**
- 安全问题: security@galay.org
- 性能回归: performance@galay.org

---

**维护者:** Galay Kernel Team
**更新频率:** 每周 (开发期间)
