# MPMC 通道性能优化总结

## 文档索引

本优化计划包含以下文档：

1. **[01_performance_analysis.md](./01_performance_analysis.md)** - 性能问题深度分析
   - 有界通道架构分析
   - 无界通道依赖问题
   - 性能瓶颈识别

2. **[02_comparison_and_bottlenecks.md](./02_comparison_and_bottlenecks.md)** - 与 Rust 对比
   - 架构差异分析
   - 关键路径对比
   - 瓶颈权重分析

3. **[03_phase1_plan.md](./03_phase1_plan.md)** - Phase 1 实施计划（2-3周）
   - 内存序优化
   - Waiter 检查优化
   - 预期收益：20-30%

4. **[04_phase2_plan.md](./04_phase2_plan.md)** - Phase 2 实施计划（3-4周）
   - 简化 Waiter 系统
   - 批量操作专用路径
   - 预期收益：额外 15-20%

5. **[05_phase3_plan.md](./05_phase3_plan.md)** - Phase 3 实施计划（4-6周）
   - 双路径架构
   - 考虑自实现
   - 预期收益：额外 10-15%

## 快速参考

### 当前性能差距

| 通道类型 | 场景 | 性能差距 | 主要原因 |
|---------|------|---------|---------|
| 有界 | 2P2C | -40% | SEQ_CST + Waiter 检查 |
| 有界 | 4P4C | -50% | + CAS 竞争 |
| 有界 | 8P8C | -60% | 竞争加剧 |
| 无界 | 2P2C | -38% | Moodycamel 包装 |
| 无界 | 4P4C | -46% | + Waiter 系统 |
| 无界 | 8P8C | -55% | Pump 开销 |

### 优化路线图

```
Baseline (v1.9)
    ↓
Phase 1: 内存序 + Waiter 优化 (+20-30%)
    ↓
Phase 2: 架构简化 + 批量操作 (+15-20%)
    ↓
Phase 3: 双路径 + 高级优化 (+10-15%)
    ↓
Target: 90-95% Rust 性能
```

### 核心发现

**有界通道主要问题:**
1. **SEQ_CST 开销 (35%)** - 每条消息多次全局同步
2. **Waiter 路径检查 (20%)** - 即使无等待者也检查
3. **Pump 系统 (20%)** - 额外的竞争点和延迟
4. **CAS 竞争 (15%)** - 退避策略不够优化
5. **其他 (10%)** - 杂项开销

**无界通道主要问题:**
1. **Moodycamel 包装 (30%)** - 第三方库调用开销
2. **Waiter 检查 (25%)** - SEQ_CST 读取
3. **Pump 系统 (20%)** - 复杂的唤醒机制
4. **Token 验证 (15%)** - 额外检查开销
5. **其他 (10%)** - TLS 查找等

## 优化策略总览

### Phase 1: 热路径优化 (P0)

**关键优化点:**

1. **降低内存序强度**
   ```cpp
   // 当前
   slot->sequence.store(pos + 1, std::memory_order_seq_cst);

   // 优化后
   slot->sequence.store(pos + 1, std::memory_order_release);
   ```
   **收益:** +15-20%

2. **优化 Waiter 检查**
   ```cpp
   // 快速路径: relaxed
   if (m_recvWaiterPathUsed.load(std::memory_order_relaxed)) {
       // 慢路径: fence + recheck
       std::atomic_thread_fence(std::memory_order_seq_cst);
       if (m_recvWaiterPathUsed.load(std::memory_order_relaxed)) {
           requestPump(kRecvWork);
       }
   }
   ```
   **收益:** +8-12%

3. **改进 CAS 退避**
   - 增加 spin limit: 6 → 10
   - 延迟 yield: 新增阈值 20
   **收益:** +5-10% (高竞争)

4. **批量操作优化**
   - 单次 CAS 认领多个 slot
   - 批量发布摊销开销
   **收益:** 批量 100 +60-80%

**总体预期:** +20-30% 吞吐

### Phase 2: 架构简化 (P1)

**关键优化点:**

1. **简化 Waiter 系统**
   - 用侵入式栈替代 moodycamel 队列
   - 直接唤醒替代 pump 系统
   - 减少动态分配
   **收益:** +10-15%

2. **批量唤醒**
   ```cpp
   void tryWakeBatchRecvWaiters(size_t maxCount) {
       WaiterNode<T>* head = m_recvWaiters.popAll();
       // 批量唤醒多个等待者
   }
   ```
   **收益:** 批量场景 +20-30%

3. **无界通道包装层优化**
   - 使用 [[likely]] / [[unlikely]]
   - 条件编译快速路径
   - Token 路径假设有效
   **收益:** +3-8%

4. **专用批量路径**
   - 有界: 批量 CAS + 批量发布
   - 无界: 使用 moodycamel bulk API
   **收益:** 批量 100 +60-100%

**总体预期:** 额外 +15-20% 吞吐

### Phase 3: 高级优化 (P2)

**关键优化点:**

1. **双路径架构**
   - 快速同步路径: 零 waiter 开销
   - 慢速异步路径: 完整功能
   **收益:** 同步场景 +10-15%

2. **评估自实现无界通道**
   - 分段链表设计
   - 针对 channel 优化
   **收益:** 可能 +15-25% (如果实施)

3. **SIMD 批量扫描**
   - AVX2 批量检查 ready flag
   **收益:** 批量场景 +15-25%

4. **Per-Core Ring (实验性)**
   - 每核心独立 ring
   - 消费者轮询
   **收益:** 8P8C +20-40% (高度实验)

**总体预期:** 额外 +10-15% 吞吐

## 预期成果

### Phase 1 完成后

**性能提升:**
- 有界 2P2C: 90M → 115M msg/s (+28%)
- 有界 4P4C: 100M → 130M msg/s (+30%)
- 无界 2P2C: 100M → 125M msg/s (+25%)
- 无界 4P4C: 130M → 162M msg/s (+25%)

**与 Rust 差距:**
- 有界通道: 从 -40% 缩小到 -23%
- 无界通道: 从 -38% 缩小到 -22%

### Phase 2 完成后

**性能提升:**
- 有界 2P2C: 115M → 138M msg/s (+20%)
- 有界 4P4C: 130M → 156M msg/s (+20%)
- 无界 2P2C: 125M → 143M msg/s (+14%)
- 无界 4P4C: 162M → 186M msg/s (+15%)

**与 Rust 差距:**
- 有界通道: 从 -23% 缩小到 -8%
- 无界通道: 从 -22% 缩小到 -12%

### Phase 3 完成后

**性能提升:**
- 有界 2P2C: 138M → 154M msg/s (+12%)
- 有界 4P4C: 156M → 176M msg/s (+13%)
- 无界 2P2C: 143M → 162M msg/s (+13%)
- 无界 4P4C: 186M → 214M msg/s (+15%)

**与 Rust 差距:**
- 有界通道: 从 -8% 优化到 +3%
- 无界通道: 从 -12% 缩小到 -5%

## 实施时间表

```
2026-08 W1-W3  │ Phase 1 实施 (有界 + 无界)
2026-08 W4     │ Phase 1 测试验证
──────────────────────────────────────
2026-09 W1-W3  │ Phase 2 实施
2026-09 W4     │ Phase 2 测试验证
──────────────────────────────────────
2026-10 W1-W4  │ Phase 3 设计和实施
2026-11 W1-W2  │ Phase 3 测试验证
──────────────────────────────────────
2026-11 W3     │ v2.0 正式发布
```

**注意:** Phase 3 为可选阶段，取决于 Phase 1+2 的成果

## 与 MPSC 的协同

### 共同优化策略

1. **内存序降级** - 相同策略
2. **Waiter 检查优化** - 相同模式
3. **Pump 系统简化** - 相同方法
4. **批量操作** - 类似实现

### 差异点

1. **MPMC 多了消费者竞争** - Head CAS
2. **MPMC 公平性要求更高** - 需要额外考虑
3. **MPMC Waiter 唤醒更复杂** - 多个接收者

### 实施建议

**顺序:**
1. 先优化 MPSC (更简单)
2. 验证策略有效性
3. 应用到 MPMC
4. 处理 MPMC 特有问题

**代码复用:**
- Waiter 栈可以共享
- 内存序优化模式可以复用
- 批量操作框架可以统一

## 关键技术决策

### 1. 是否替换 Moodycamel？

**短期 (Phase 1-2):** 不替换
- 专注于优化包装层
- 快速见效
- 风险低

**长期 (Phase 3):** 评估替换
- 如果差距仍 >15%，考虑自实现
- 需要充分的性能证明
- 代价高，谨慎决策

**决策标准:**
- 自实现必须比 moodycamel+包装 快 ≥15%
- 正确性充分验证
- 代码可维护

### 2. 双路径 vs 单一路径？

**选择:** 双路径架构

**理由:**
- 同步路径达到最优性能
- 异步功能完整保留
- API 向后兼容
- 用户可以选择使用哪个

**权衡:**
- 增加代码复杂度
- 需要维护两套实现
- 但性能收益值得

### 3. Waiter 队列的实现？

**Phase 1:** 保留 moodycamel 队列
**Phase 2:** 替换为侵入式栈
**Phase 3:** 可能引入更复杂的结构

**选择侵入式栈的理由:**
- 零动态分配
- 更简单的所有权
- 更快的 push/pop
- 足够满足需求

**权衡:**
- LIFO 而非 FIFO
- 需要验证公平性
- 可通过定期重排缓解

## 风险和缓解

### 技术风险

**风险 1: 内存序降级导致 Race**

**缓解:**
- ThreadSanitizer 充分测试
- 多平台验证 (x86, ARM)
- 压力测试 72 小时+
- 逐步降级，观察结果

**风险 2: 自实现无界通道复杂度高**

**缓解:**
- Phase 3 作为可选
- 充分原型验证
- 与 moodycamel 性能对比
- 可随时回退

**风险 3: 双路径增加维护负担**

**缓解:**
- 共享核心逻辑
- 充分的单元测试
- 清晰的文档
- 代码审查严格

### 项目风险

**风险 1: 时间延期**

**缓解:**
- 分阶段交付
- Phase 1 优先
- Phase 3 可延后
- 并行开发

**风险 2: 性能不达预期**

**缓解:**
- 早期基准测试
- 持续性能监控
- 及时调整策略
- 保留回退方案

## 测试策略

### 正确性测试

**必须通过:**
- ✅ 所有单元测试
- ✅ ThreadSanitizer 零告警
- ✅ AddressSanitizer 零告警
- ✅ 压力测试 72 小时无故障
- ✅ 多平台验证 (x86, ARM)

**测试场景:**
- 单生产者单消费者
- 多生产者单消费者
- 单生产者多消费者
- 多生产者多消费者
- 混合批量和单条
- 高竞争场景
- 关闭和超时

### 性能测试

**基准测试矩阵:**
```
Topologies: 1P1C, 2P1C, 1P2C, 2P2C, 4P4C, 8P8C
Capacities (有界): 64, 256, 1024, 4096
Message sizes: 8B, 64B, 256B, 1024B
Batch sizes: 1, 10, 50, 100, 500, 1000
Contention: low, medium, high
```

**对比基准:**
- Galay Phase N vs Phase N-1
- Galay vs Rust crossbeam
- 不同 topology 和参数组合

**验收标准:**
- Phase 1: +20% vs baseline
- Phase 2: +15% vs Phase 1
- Phase 3: +10% vs Phase 2
- 无性能回归

## 监控和观测

### 开发期间

| 指标 | 频率 | 阈值 |
|------|------|------|
| 单元测试通过率 | 每次提交 | 100% |
| TSan/ASan 告警 | 每次提交 | 0 |
| 性能提升 | 每周 | 按计划 |
| 代码覆盖率 | 每周 | >85% |

### 发布后

| 指标 | 频率 | 阈值 |
|------|------|------|
| Crash 率 | 每天 | <0.01% |
| 性能回归报告 | 每天 | 0 |
| 内存泄漏 | 每周 | 0 |
| 用户反馈 | 每周 | >90% 处理 |

## 参考资料

### 论文和文章

1. **Dmitry Vyukov MPMC Queue**
   - http://www.1024cores.net/home/lock-free-algorithms/queues/bounded-mpmc-queue
   - 有界 MPMC 经典算法

2. **Moodycamel ConcurrentQueue**
   - https://github.com/cameron314/concurrentqueue
   - 高性能无界 MPMC 实现

3. **Rust Crossbeam Channel**
   - https://github.com/crossbeam-rs/crossbeam
   - 参考实现

### 相关项目

1. **Folly MPMCQueue**
   - https://github.com/facebook/folly
   - Facebook 的实现

2. **Boost Lockfree**
   - https://www.boost.org/doc/libs/release/libs/lockfree/
   - Boost 无锁队列

## 贡献指南

### 参与优化

**选择任务:**
- GitHub Issues 标签 `mpmc-optimization`
- Phase 1 适合新手
- Phase 2-3 需要并发经验

**开发流程:**
```bash
# 创建特性分支
git checkout -b feature/mpmc-phase1-memory-order

# 开发和测试
make test-mpmc

# 提交 PR
gh pr create --title "MPMC Phase 1: Optimize memory order"
```

**审查要求:**
- 必须通过 TSan/ASan
- 必须包含单元测试
- 必须有性能基准对比
- 必须通过两位 reviewer

### 报告问题

**性能问题:**
- 使用模板 `.github/ISSUE_TEMPLATE/performance.md`
- 包含完整基准数据
- 提供可重现的测试

**正确性问题:**
- 优先级最高
- 提供最小复现用例
- 包含 TSan/ASan 输出

## 联系方式

**问题和讨论:**
- GitHub Issues: https://github.com/galay/galay/issues
- 邮件列表: dev@galay.org
- Slack: #mpmc-optimization

**性能问题报告:**
- 标签: `performance`, `mpmc`
- 包含基准数据和环境信息

---

**文档版本:** v1.0
**最后更新:** 2026-08-04
**维护者:** Galay Kernel Team
