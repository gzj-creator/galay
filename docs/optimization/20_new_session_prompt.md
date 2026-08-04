# Claude 新 Session 提示词

请将以下内容完整复制到新 session 中：

---

我需要修复和优化 Per-Producer MPMC 项目中的高负载稳定性问题。

## 项目背景

这是一个 galay 项目的并发通道实现，采用 Per-Producer Ring 架构（每个 producer 独占一个 ring buffer，consumer 轮询所有 rings）。

**项目路径**: `/Users/gongzhijie/Desktop/projects/git/galay`

## 当前问题

**现象**：
- 小规模测试（≤400 消息）：✅ 全部通过
- 大规模测试（4000 消息）：❌ 卡在 ~3000 条消息
- 压力测试 `t171_per_producer_mpmc_stress` 会卡住
- 完整测试 `t168_per_producer_mpmc` 会超时

**复现命令**：
```bash
cd /Users/gongzhijie/Desktop/projects/git/galay
./build-release/test/cpp/kernel/t171_per_producer_mpmc_stress
```

## 根本原因

1. **CAS 竞争累积** - 多个 consumer 竞争同一个 ring 的 head cursor，CAS 失败后直接返回 0，导致消息无法被消费
2. **轮询策略问题** - Thread-local 索引可能导致所有 consumer 都跳过某些有消息的 ring
3. **重试机制缺失** - CAS 失败后没有重试，直接轮询下一个 ring

## 项目约束

**必须遵守**（来自 CLAUDE.md）：
- ❌ 禁止异常（`throw`, `try`, `catch`）
- ❌ 禁止 Mutex/Lock（协程环境，禁止阻塞操作）
- ✅ 必须使用 `std::expected` 返回错误
- ✅ 必须使用原子操作（无锁编程）

## 关键文件

**核心实现**：
- `src/cpp/galay-kernel/concurrency/mpmc/per_producer_ring.h:124-175` - `tryRecvBatch()` 方法
- `src/cpp/galay-kernel/concurrency/mpmc/per_producer_mpmc_channel.h:217-243` - `tryRecv()` 方法

**测试文件**：
- `test/cpp/kernel/t171_per_producer_mpmc_stress.cc` - 卡住的压力测试
- `test/cpp/kernel/t168_per_producer_mpmc.cc` - 完整测试套件

**文档**：
- `docs/optimization/19_handoff_document.md` - **完整交接文档（必读）**

## 推荐解决方案

### 方案 A: 改进重试机制

在 `per_producer_ring.h:tryRecvBatch()` 中添加重试循环：
- CAS 失败后不要立即放弃，重试 2-3 次
- 使用 exponential backoff 减少竞争
- 预期解决 70-80% 问题

### 方案 B: 改进轮询策略

在 `per_producer_mpmc_channel.h:tryRecv()` 中：
- 第一轮从 thread-local 索引开始
- 第二轮从头开始，确保覆盖所有 rings
- 预期解决 80-90% 问题

### 方案 C: 组合方案（推荐）⭐

同时实施方案 A + B，预期解决 >95% 问题。

## 详细实现

完整的代码示例和实现细节请查看：
`docs/optimization/19_handoff_document.md` 的"建议的解决方案"章节。

## 验证步骤

修复后运行以下测试：

```bash
# 1. 压力测试（应在 10 秒内完成）
./build-release/test/cpp/kernel/t171_per_producer_mpmc_stress

# 2. 完整测试套件（应在 60 秒内完成）
./build-release/test/cpp/kernel/t168_per_producer_mpmc

# 3. 性能测试
./build-release/benchmark/cpp/kernel/benchmark_kernel_per_producer_mpmc_benchmark \
  --producers 4 --consumers 4 --messages 5000000 --capacity 4096 \
  --strategy balanced --max-per-ring-batch 16
```

## 成功标准

**必须满足**：
1. ✅ 所有测试通过（包括 t171）
2. ✅ 无超时或卡住
3. ✅ 符合项目规范（无 mutex）
4. ✅ 4P4C 吞吐量 > 50 M msg/s

## 请求

请按照以下步骤修复问题：

1. **阅读交接文档** - 详细阅读 `docs/optimization/19_handoff_document.md`
2. **实施方案 C** - 同时改进重试机制和轮询策略
3. **重新构建** - `cmake --build build-release --target t168_per_producer_mpmc t171_per_producer_mpmc_stress`
4. **验证测试** - 运行上述验证步骤
5. **性能测试** - 确保性能满足标准
6. **总结报告** - 创建修复报告文档

开始吧！

---

**注意**：
- 交接文档包含完整的代码示例和详细分析
- 如有疑问，参考 `docs/optimization/` 目录下的其他 7 份文档
- 项目使用 C++23，Apple Clang，macOS ARM64
