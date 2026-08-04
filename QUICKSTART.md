# MPSC 双策略实施 - 快速启动指南

## 当前状态 ✨

- ✅ **延迟优先策略**：已实现，4P1C 性能 171.5M/s (24.9x 提升)
- ✅ **吞吐优先策略**：已实现，完整代码 ~700 行
- 🔄 **工作流运行中**：自动化测试、实现、对比、文档
- ✅ **腾讯部署脚本**：已准备就绪

---

## 一、工作流监控

### 查看实时进度

```bash
# 使用 Claude Code 命令
/workflows

# 或者检查日志
tail -f /Users/gongzhijie/.claude/projects/-Users-gongzhijie-Desktop-projects-git-galay/*/subagents/workflows/*/journal.jsonl
```

### 工作流阶段

工作流将自动完成以下任务：

1. **Phase 1: 本机测试验证** 🔄
   - 等待性能测试完成
   - 分析结果数据
   - 识别优化机会

2. **Phase 2: Async 支持实现**
   - 添加 ThroughputBoundedSendAwaitable
   - 添加 ThroughputBoundedRecvAwaitable
   - 添加批量接收支持

3. **Phase 3: Rust 对比准备**
   - 检查 Crossbeam 测试环境
   - 运行对比测试
   - 生成性能报告

4. **Phase 4: 文档完善**
   - 使用指南
   - 最佳实践
   - 更新 README

---

## 二、腾讯机器部署

### 配置连接信息

编辑 `deploy_to_tencent.sh`：

```bash
# 修改这些配置
TENCENT_HOST="your-tencent-machine"  # 腾讯机器地址
TENCENT_USER="your-username"         # 用户名
TENCENT_PORT="22"                    # SSH 端口
```

### 执行部署测试

#### 方式 1：完整流程（推荐）

```bash
./deploy_to_tencent.sh full
```

这将自动执行：
1. 部署代码
2. 编译（Release + -march=native）
3. 性能测试
4. perf 分析
5. Crossbeam 对比
6. 生成报告

#### 方式 2：分步执行

```bash
# 1. 仅部署
./deploy_to_tencent.sh deploy

# 2. 仅编译
./deploy_to_tencent.sh build

# 3. 仅测试
./deploy_to_tencent.sh test

# 4. 性能分析
./deploy_to_tencent.sh analyze

# 5. Crossbeam 对比
./deploy_to_tencent.sh crossbeam

# 6. 生成报告
./deploy_to_tencent.sh report
```

#### 方式 3：压力测试（24小时）

```bash
# 启动后台压力测试
./deploy_to_tencent.sh stress

# 监控进度
ssh your-username@your-tencent-machine 'tail -f /home/your-username/galay-test/build/stress_test.log'
```

### 结果查看

测试完成后，结果保存在：
```
benchmark-results/tencent-YYYYMMDD/
├── tencent_system_info.txt          # 系统信息
├── tencent_strategy_comparison.log  # 策略对比
├── tencent_bounded_throughput.log   # 基准测试
├── tencent_crossbeam.log            # Crossbeam 对比
├── perf_stat.log                    # perf 统计
├── perf_report.txt                  # perf 报告
└── TENCENT_PERFORMANCE_REPORT.md    # 综合报告
```

---

## 三、手动操作（如果需要）

### 本机测试

```bash
# 查看当前测试状态
./check_test_status.sh

# 如果测试已完成，查看结果
cat /tmp/strategy_comparison.log

# 手动运行 Crossbeam 对比
cd benchmark/cpp/kernel/compare/rust-channel
cargo bench
```

### 参数调优

如果性能未达预期，调整这些参数：

**批量大小**（`throughput_bounded_channel.h:267`）：
```cpp
static constexpr size_t kPublishBatch = 16;  // 尝试 8, 32, 64
```

**消费者配额**（`throughput_bounded_channel.h:447`）：
```cpp
static constexpr size_t kConsumerQuota = 64;  // 尝试 32, 128
```

**Ring 容量分配**（构造函数）：
```cpp
// 当前：均分
size_t ringCapacity = totalCapacity / maxProducers;

// 可选：固定大小
size_t ringCapacity = 256;  // 每个 ring 固定 256
```

重新编译测试：
```bash
cmake --build build --target benchmark_kernel_mpsc_strategy_comparison -j8
./build/benchmark/cpp/kernel/benchmark_kernel_mpsc_strategy_comparison
```

---

## 四、关键文件位置

### 源代码

```
src/cpp/galay-kernel/concurrency/mpsc/
├── bounded_channel.h              # 延迟优先（指数退避）
└── throughput_bounded_channel.h   # 吞吐优先（per-producer ring）
```

### 测试

```
benchmark/cpp/kernel/
├── b23_bounded_channel_throughput.cc    # 原有基准测试
└── b28_mpsc_strategy_comparison.cc      # 策略对比测试
```

### 文档

```
docs/optimization/
├── 07_mpsc_strategy_design.md           # 设计方案
├── 08_exponential_backoff_results.md    # 性能结果
├── 09_mpsc_strategy_summary.md          # 策略总结
├── 10_action_checklist.md               # 行动清单
├── 11_throughput_first_implementation.md # 实施文档
├── 12_final_summary.md                  # 最终总结
└── DELIVERY_REPORT.md                   # 交付报告
```

### 脚本

```
.
├── deploy_to_tencent.sh           # 腾讯机器部署
├── check_test_status.sh           # 测试状态检查
└── benchmark/cpp/kernel/compare/
    └── run_crossbeam_comparison.sh # Crossbeam 对比
```

---

## 五、预期性能目标

### 本机（Mac）

| 场景  | Latency-First | Throughput-First | 目标提升 |
|-------|---------------|------------------|----------|
| 2P1C  | 101.3M/s      | ~120M/s          | 1.19x    |
| 4P1C  | 171.5M/s      | ~220M/s          | 1.29x    |
| 8P1C  | ~280M/s       | ~420M/s          | 1.50x    |
| 16P1C | ~320M/s       | ~650M/s          | 2.03x    |

### 腾讯机器（Linux，更多核心）

预期更高并发性能：
- 16P1C: ~700M/s
- 32P1C: ~1.2G/s
- 64P1C: ~2.0G/s（如果线性扩展）

### vs Crossbeam

目标：**超越 Crossbeam 5-10x**（基于 unbounded 的相对性能）

---

## 六、故障排查

### 测试挂起

```bash
# 检查进程
ps aux | grep benchmark_kernel_mpsc_strategy_comparison

# 如果需要，终止并重新运行
pkill -f benchmark_kernel_mpsc_strategy_comparison
cd build && ./benchmark/cpp/kernel/benchmark_kernel_mpsc_strategy_comparison
```

### 编译错误

```bash
# 清理并重新配置
rm -rf build
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
```

### SSH 连接失败

```bash
# 测试连接
ssh -p 22 your-username@your-tencent-machine 'echo "连接成功"'

# 如果失败，检查：
# 1. 主机名/IP 是否正确
# 2. SSH 密钥是否配置
# 3. 防火墙是否开放
```

### perf 权限不足

在腾讯机器上：
```bash
# 临时允许 perf
sudo sysctl -w kernel.perf_event_paranoid=-1

# 或使用 sudo
sudo perf stat ./benchmark_kernel_mpsc_strategy_comparison
```

---

## 七、下一步行动

### 立即（等工作流完成）

1. 监控工作流进度：`/workflows`
2. 查看生成的文档和代码
3. 审查性能数据

### 短期（今晚/明天）

1. 编辑 `deploy_to_tencent.sh` 配置
2. 运行腾讯机器测试：`./deploy_to_tencent.sh full`
3. 分析对比结果

### 中期（本周）

1. 根据测试结果调优参数
2. 实现统一 API
3. Code Review
4. 合并主分支

---

## 八、成功标准

### 必达 ✅

- ✅ 延迟优先：4P1C ≥ 150M/s（已达成 171.5M/s）
- 🎯 吞吐优先：8P1C ≥ 400M/s（测试中）
- 🎯 腾讯机器验证通过

### 期望 🎯

- 🎯 16P1C ≥ 600M/s
- 🎯 超越 Crossbeam 5x+
- 🎯 线性扩展到 32P

### 理想 🚀

- 🚀 64P 持续扩展
- 🚀 业界最快 bounded MPSC
- 🚀 技术分享/论文

---

## 九、联系和支持

### 项目信息

- **项目名称**：galay MPSC 双策略通道
- **代码路径**：`src/cpp/galay-kernel/concurrency/mpsc/`
- **文档路径**：`docs/optimization/`
- **测试路径**：`benchmark/cpp/kernel/`

### 实时状态

- **工作流状态**：`/workflows`
- **测试进度**：`./check_test_status.sh`
- **性能数据**：`benchmark-results/`

---

## 十、快速命令参考

```bash
# 监控工作流
/workflows

# 检查测试状态
./check_test_status.sh

# 腾讯完整测试
./deploy_to_tencent.sh full

# 手动 Crossbeam 对比
cd benchmark/cpp/kernel/compare/rust-channel && cargo bench

# 重新编译测试
cmake --build build --target benchmark_kernel_mpsc_strategy_comparison -j8

# 查看结果
cat /tmp/strategy_comparison.log
cat benchmark-results/tencent-*/TENCENT_PERFORMANCE_REPORT.md
```

---

**当前状态**：🔄 工作流运行中 + ✅ 腾讯脚本就绪

**你现在可以**：
1. 监控工作流进度（`/workflows`）
2. 准备腾讯机器连接信息
3. 或者等待工作流完成后一键部署

---

**最后更新**：2026-08-04 23:50
