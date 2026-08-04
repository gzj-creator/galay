# Pull Request 模板

## PR 标题格式

```
<type>: <简洁描述> (<70 字符)

示例:
feat: 实现 MPSC 双策略通道架构（延迟/吞吐优先）
perf: 降低有界通道内存序强度提升 20% 吞吐
fix: 修复无界通道 TLS 缓存竞争导致的性能回退
refactor: 简化 waiter 系统移除 pump 间接层
```

**类型:**
- `feat` - 新特性
- `perf` - 性能优化
- `fix` - Bug 修复
- `refactor` - 重构
- `test` - 测试相关
- `docs` - 文档更新

---

## 1. 背景和动机

### 当前问题

**问题描述:**
<!-- 清晰描述要解决的问题，用数据说话 -->

示例:
```
当前 MPSC 有界通道在 4P1C 场景下吞吐量为 60M msg/s，
相比 Rust crossbeam 的 120M msg/s 慢 50%。

主要瓶颈:
- 热路径使用 seq_cst 内存序，每条消息 2-3 次全局屏障
- 等待者计数检查即使无等待者也要执行 seq_cst 读取
- 多生产者竞争 waiter 计数成为热点
```

**性能数据 (优化前):**
<!-- 提供基准测试数据 -->

| 拓扑 | 当前吞吐 | Crossbeam 吞吐 | 差距 |
|------|---------|---------------|------|
| 2P1C | 80M/s   | 140M/s        | -43% |
| 4P1C | 60M/s   | 120M/s        | -50% |
| 8P1C | 35M/s   | 90M/s         | -61% |

**业务影响:**
<!-- 这个问题如何影响用户或系统 -->

---

## 2. 解决方案

### 架构变更

**核心设计:**
<!-- 高层次描述解决方案，避免代码细节 -->

示例:
```
采用 Release/Acquire 内存序替代 seq_cst:
1. slot sequence 发布使用 memory_order_release
2. 消费者读取使用 memory_order_acquire
3. waiter 检查使用 relaxed + fence 双重检查

理论依据:
- Release/Acquire 提供充分的跨线程可见性保证
- 不需要 seq_cst 的全局顺序一致性
- x86 平台 Release/Acquire 是编译器屏障，零运行时成本
```

**关键组件:**
<!-- 列出涉及的主要文件和模块 -->

- `src/cpp/galay-kernel/concurrency/mpsc/bounded_channel.h`
  - `trySend()` 内存序降级
  - `tryRecv()` 内存序匹配
  - waiter 检查优化

**数据流变化:**
<!-- 用简图或伪代码说明关键路径变化 -->

```
优化前:
Producer -> SEQ_CST store -> SEQ_CST waiter check -> SEQ_CST pump request
Consumer -> SEQ_CST load -> SEQ_CST waiter update

优化后:
Producer -> RELEASE store -> RELAXED waiter check (fast path) -> fence (slow path)
Consumer -> ACQUIRE load -> RELAXED waiter update
```

**权衡分析:**
<!-- 诚实说明优化带来的权衡 -->

| 维度 | 收益 | 代价 |
|------|------|------|
| 性能 | +20-30% 吞吐 | - |
| 正确性 | 不变（语义保持） | 更复杂的内存模型推理 |
| 可维护性 | 简化热路径 | 需要更详细的注释 |
| 可移植性 | 不变 | ARM 平台需验证 |

---

## 3. 性能数据

### 基准测试结果

**测试环境:**
```
CPU: Intel Xeon Gold 6240 @ 2.6GHz (36 cores)
OS: Linux 5.15.0 x86_64
Compiler: GCC 11.3.0 -O3 -march=native
Rust: 1.75.0
```

**吞吐量对比:**

| 拓扑 | 优化前 | 优化后 | 提升 | Crossbeam | 差距 |
|------|--------|--------|------|-----------|------|
| 2P1C | 80M/s  | 105M/s | +31% | 140M/s    | -25% |
| 4P1C | 60M/s  | 82M/s  | +37% | 120M/s    | -32% |
| 8P1C | 35M/s  | 48M/s  | +37% | 90M/s     | -47% |

**延迟分析 (2P1C, 1M msg):**

| 指标 | 优化前 | 优化后 | 改善 |
|------|--------|--------|------|
| P50  | 120ns  | 95ns   | -21% |
| P99  | 450ns  | 320ns  | -29% |
| P999 | 2.1μs  | 1.5μs  | -29% |

**CPU 性能计数器 (perf stat):**
```bash
# 优化前
Performance counter stats for './mpsc_paired_baseline 4 1':
  28,450,234,891  cycles
   6,892,345,123  cache-misses
   2,145,678,901  bus-locks

# 优化后
Performance counter stats for './mpsc_paired_phase1 4 1':
  20,123,456,789  cycles              (-29%)
   5,234,567,890  cache-misses        (-24%)
     987,654,321  bus-locks           (-54%)  <-- 关键改善
```

**火焰图分析:**
<!-- 附上 perf 火焰图链接或关键热点对比 -->

```
优化前热点:
- atomic_store_seq_cst: 35%
- waiter_count_check: 18%
- pump_request: 12%

优化后热点:
- atomic_store_release: 12%  (-66%)
- waiter_count_check: 8%     (-56%)
- actual_message_copy: 25%   (成为主要开销，符合预期)
```

---

## 4. 测试计划

### 4.1 单元测试

**新增测试:**
```cpp
// test/kernel/t170_mpsc_memory_order.cc

TEST(MPSCMemoryOrder, ReleaseAcquireSufficient) {
  // 验证 Release/Acquire 提供充分的同步
  // 使用多线程写入不同字段，验证消费者能看到完整状态
}

TEST(MPSCMemoryOrder, WaiterCheckRace) {
  // 压力测试 waiter 检查的 relaxed + fence 逻辑
  // 确保不会丢失唤醒
}

TEST(MPSCMemoryOrder, NoSpuriousWakeup) {
  // 验证双重检查确实减少虚假唤醒
}
```

**回归测试:**
```bash
# 运行所有现有 MPSC 测试
./build/test/kernel/t151_channel_namespaces
./build/test/kernel/t154_mpsc_unbounded_source
./build/test/kernel/t156_mpsc_timeout_race
./build/test/kernel/t163_mpsc_redesign
./build/test/kernel/t166_spsc_paired_final_drain_source

# 验收标准: 100% 通过
```

### 4.2 并发测试

**TSan (Thread Sanitizer):**
```bash
# 编译 TSan 版本
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_CXX_FLAGS="-fsanitize=thread -g" \
      ..

# 运行完整测试套件
./build/test/kernel/t163_mpsc_redesign --gtest_repeat=1000

# 验收标准: 零 race 告警
```

**压力测试:**
```bash
# 24 小时压力测试
./test/stress/mpsc_stress_test \
    --duration=86400 \
    --producers=8 \
    --consumers=1 \
    --messages=1000000000

# 监控指标:
# - 吞吐量稳定
# - 无内存泄漏
# - 无死锁
# - 无消息丢失
```

### 4.3 性能基准

**对比测试矩阵:**

| 拓扑 | 容量 | 消息数 | 验收标准 |
|------|------|--------|---------|
| 2P1C | 4096 | 100M   | +25% 以上 |
| 4P1C | 4096 | 100M   | +30% 以上 |
| 8P1C | 4096 | 100M   | +35% 以上 |

**跨平台验证:**
```bash
# 本地 x86 验证
./benchmark/cpp/kernel/compare/run_mpsc_paired.py --output-dir ./results/x86

# ARM 平台验证 (腾讯云机器)
ssh tencent-arm ./benchmark/run_remote.sh

# 验收标准: 两个平台都达到提升目标
```

### 4.4 Crossbeam 对比

**完整场景覆盖:**
```bash
# 运行标准基准套件
./benchmark/cpp/kernel/compare/run_mpsc_paired.py \
    --cpp-binary ./build/mpsc_paired_optimized \
    --rust-binary ./rust-channel/target/release/mpsc_paired \
    --output-dir ./results/phase1_vs_crossbeam \
    --producers 1,2,4,8,16 \
    --capacity 256,1024,4096,16384

# 生成对比报告
python3 ./benchmark/generate_report.py ./results/phase1_vs_crossbeam
```

---

## 5. 风险评估

### 5.1 正确性风险

**R1: 内存序降级导致可见性问题**

**风险等级:** 🔴 HIGH

**描述:**
- Release/Acquire 在某些极端场景可能无法提供足够同步
- ARM 平台的弱内存模型可能暴露新的 race

**缓解措施:**
- ✅ 详细的内存模型推理文档
- ✅ TSan 多次运行 (1000+ 次)
- ✅ ARM 平台实机验证
- ✅ 形式化验证工具 (Relacy, CDSChecker)

**应急方案:**
- 如果发现 race，立即回滚到 seq_cst
- 使用 fence 增强同步而非放弃优化

---

**R2: Waiter 检查 relaxed 导致丢失唤醒**

**风险等级:** 🟡 MEDIUM

**描述:**
- 双重检查的窗口期可能导致 waiter 错过唤醒
- 虽然最终会被超时或其他消息唤醒，但延迟增加

**缓解措施:**
- ✅ fence 保证可见性
- ✅ 压力测试覆盖高负载下的 waiter 场景
- ✅ 延迟分布监控 (P99, P999)

**应急方案:**
- 如果延迟回退，回退到 seq_cst waiter 检查

---

### 5.2 性能风险

**R3: ARM 平台性能不达预期**

**风险等级:** 🟡 MEDIUM

**描述:**
- ARM 的 Release/Acquire 仍然有运行时开销
- 提升幅度可能小于 x86

**缓解措施:**
- ✅ 早期 ARM 验证
- ✅ ARM 特定优化路径 (条件编译)

**验收标准:**
- x86: +30% 以上
- ARM: +15% 以上 (可接受)

---

**R4: 回退风险**

**风险等级:** 🟢 LOW

**描述:**
- 优化后某些场景性能下降

**缓解措施:**
- ✅ 完整基准矩阵覆盖
- ✅ 保留 seq_cst 版本作为编译选项

---

### 5.3 兼容性风险

**R5: ABI 破坏**

**风险等级:** 🟢 LOW

**描述:**
- 内存序修改不影响 ABI
- 只是内联函数的实现变化

**验收:**
- ✅ 符号表对比无变化
- ✅ 动态库可替换升级

---

## 6. Review 检查清单

### 6.1 代码质量

**通用标准:**
- [ ] 代码风格符合项目规范 (clang-format)
- [ ] 所有修改都有清晰注释
- [ ] 复杂逻辑有内存模型推理说明
- [ ] 无 TODO/FIXME 遗留
- [ ] 函数长度合理 (<100 行)
- [ ] 无深层嵌套 (>4 层)

**性能关键路径:**
- [ ] 热路径无不必要的分支
- [ ] 无隐藏的内存分配
- [ ] 原子操作使用最弱的充分内存序
- [ ] 无虚假共享 (cache line 对齐检查)

### 6.2 线程安全

**内存模型审查:**
- [ ] 所有原子操作的内存序有注释说明理由
- [ ] Release/Acquire 配对正确
- [ ] Relaxed 操作有 fence 保护或无需同步
- [ ] 跨线程数据依赖链完整

**并发场景覆盖:**
- [ ] 多生产者竞争场景
- [ ] 生产者-消费者交错场景
- [ ] 关闭与发送竞争场景
- [ ] Waiter 唤醒与消息到达竞争

### 6.3 测试覆盖

**单元测试:**
- [ ] 新增代码有对应单元测试
- [ ] 测试覆盖率 >90%
- [ ] 边界条件测试完整
- [ ] 异常路径测试

**集成测试:**
- [ ] 所有现有测试通过
- [ ] TSan 零告警
- [ ] ASan 零告警
- [ ] 压力测试 24 小时无故障

**性能测试:**
- [ ] 基准测试数据完整
- [ ] Crossbeam 对比数据
- [ ] 多平台验证 (x86, ARM)
- [ ] 性能计数器分析

### 6.4 文档完整

**代码文档:**
- [ ] 公开 API 有 Doxygen 注释
- [ ] 内存模型决策有详细注释
- [ ] 复杂算法有伪代码或引用

**设计文档:**
- [ ] PR 描述包含完整设计说明
- [ ] 性能数据图表清晰
- [ ] 权衡分析透明

**变更日志:**
- [ ] CHANGELOG.md 更新
- [ ] 版本号符合语义化版本规范
- [ ] 迁移指南 (如有 API 变更)

---

## 7. 合并前检查清单

### 7.1 功能验收

- [ ] 所有 CI 检查通过 (编译、测试、linting)
- [ ] 本地完整测试套件 100% 通过
- [ ] TSan/ASan 零告警
- [ ] 压力测试 24 小时无故障

### 7.2 性能验收

**验收拓扑:** 2P1C、4P1C、8P1C

- [ ] 2P1C 吞吐提升 ≥ 25%
- [ ] 4P1C 吞吐提升 ≥ 30%
- [ ] 8P1C 吞吐提升 ≥ 35%
- [ ] P99 延迟无回退
- [ ] Crossbeam 差距缩小 15-20%

**多平台验证:**
- [ ] x86 平台达标
- [ ] ARM 平台验证 (腾讯云机器)
- [ ] 两个平台性能趋势一致

### 7.3 代码审查

- [ ] 至少 2 位 reviewer 批准
- [ ] 所有 review 意见已解决
- [ ] 无未解决的讨论线程
- [ ] 架构组 (如有重大变更) 批准

### 7.4 文档同步

- [ ] 代码注释完整
- [ ] 设计文档更新
- [ ] CHANGELOG.md 更新
- [ ] 优化计划文档状态更新
- [ ] 相关 Issue 关闭

### 7.5 回归风险

- [ ] 无新增 TODO/FIXME
- [ ] 无调试代码遗留 (cout, printf)
- [ ] 无临时 workaround 未清理
- [ ] 错误处理完整

### 7.6 发布准备

- [ ] 版本号确定 (遵循语义化版本)
- [ ] Release notes 草稿
- [ ] 性能数据汇总报告
- [ ] 迁移指南 (如需要)

---

## 8. 合并后任务

### 8.1 监控

- [ ] 性能监控 dashboard 更新
- [ ] 添加关键指标告警 (吞吐回退 >5%)
- [ ] 错误率监控

### 8.2 文档发布

- [ ] 更新官方文档网站
- [ ] 发布性能优化博客
- [ ] 社区通告

### 8.3 后续计划

- [ ] 创建 Phase 2 优化 Issue
- [ ] 总结经验教训
- [ ] 更新优化 roadmap

---

## 参考资源

### 相关文档

- [MPSC 性能分析](./01_performance_analysis.md)
- [Phase 1 优化计划](./03_phase1_plan.md)
- [风险管理](./06_risks_and_mitigation.md)
- [内存模型指南](../guides/memory_model.md)

### 基准测试

- 基准脚本: `benchmark/cpp/kernel/compare/run_mpsc_paired.py`
- 结果目录: `benchmark-results/mpsc-phase1/`
- 对比工具: `benchmark/compare_results.py`

### CI/CD

- GitHub Actions: `.github/workflows/mpsc-optimization.yml`
- 性能回归检测: `.github/workflows/perf-regression.yml`

---

**模板版本:** v1.0
**最后更新:** 2026-08-04
**维护者:** Galay Kernel Team
