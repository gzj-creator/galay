# 腾讯机器测试套件使用说明

> 正式外部对标只使用 Boost.Asio C++ 协程。channel、NUMA 和策略测试属于
> Galay 内部性能验证；Crossbeam 资料仅保留为历史归档，不参与竞品排名。

本目录包含三个用于腾讯机器性能测试的完整脚本套件。

## 📋 脚本概览

| 脚本 | 用途 | 预计耗时 |
|------|------|---------|
| `tencent_full_test.sh` | 完整性能测试流程 | 30-60 分钟 |
| `tencent_numa_test.sh` | NUMA 感知性能测试 | 20-40 分钟 |
| `tencent_perf_analysis.sh` | perf 深度性能分析 | 15-30 分钟 |

## 🚀 快速开始

### 前置要求

```bash
# 1. 确保项目已构建
cd /path/to/galay
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# 2. 安装依赖工具 (可选)
sudo yum install numactl perf  # CentOS/RHEL
# 或
sudo apt-get install numactl linux-tools-common  # Ubuntu/Debian

# 3. 调整 perf 权限 (可选,需要 root)
sudo sysctl kernel.perf_event_paranoid=-1
```

### 运行测试

```bash
# 1. 完整测试套件 (推荐首次运行)
./scripts/tencent_full_test.sh

# 2. NUMA 感知测试
./scripts/tencent_numa_test.sh

# 3. perf 性能分析
./scripts/tencent_perf_analysis.sh
```

## 📊 测试内容详解

### 1. tencent_full_test.sh - 完整测试流程

**功能:**
- ✅ 系统信息收集 (CPU/内存/NUMA 拓扑)
- ✅ 标准性能测试 (1P, 2P, 4P, 8P, 16P, 32P, 64P)
- ✅ 内部策略验证 (fair/balanced/throughput)
- ✅ Boost.Asio 协程 UDP 公平对标
- ✅ Boost.Asio 协程 TCP 公平对标
- ✅ 自动生成汇总报告

**输出文件:**
```
benchmark-results/tencent-full-YYYYMMDD-HHMMSS/
├── system_info.txt              # 系统信息
├── standard_tests.jsonl         # 标准测试结果 (JSON Lines)
├── strategy_comparison.jsonl    # 内部策略验证结果
├── boost_asio_coro_comparison.txt      # Galay / Boost.Asio UDP 交替原始输出
├── boost_asio_coro_tcp_comparison.txt  # Galay / Boost.Asio TCP 交替原始输出
├── boost_asio_coro.csv                  # UDP 机器可读结果
├── boost_asio_coro_tcp.csv              # TCP 机器可读结果
├── summary.txt                  # 汇总报告
└── full_test.log                # 完整日志
```

**关键指标:**
- 消息吞吐量 (messages/second)
- 延迟分布
- FIFO 正确性
- 重试次数

---

### 2. tencent_numa_test.sh - NUMA 感知测试

**功能:**
- ✅ NUMA 拓扑检测和可视化
- ✅ 同节点 vs 跨节点性能对比
- ✅ CPU 亲和性影响测试
- ✅ 内存分配策略对比 (default/interleave/preferred/local)

**输出文件:**
```
benchmark-results/tencent-numa-YYYYMMDD-HHMMSS/
├── numa_topology.txt       # NUMA 拓扑信息
├── same_node.jsonl         # 同节点测试结果
├── cross_node.jsonl        # 跨节点测试结果
├── affinity.jsonl          # 亲和性测试结果
├── memory_policy.jsonl     # 内存策略测试结果
├── numa_summary.txt        # NUMA 分析报告
└── numa_test.log           # 完整日志
```

**测试场景:**
```bash
# 同节点测试 - 在每个 NUMA 节点上独立运行
numactl --cpunodebind=0 --membind=0 benchmark

# 跨节点测试 - 跨多个 NUMA 节点
numactl --cpunodebind=0,1 --membind=0 benchmark

# 内存交错 - 均匀分布内存访问
numactl --interleave=all benchmark
```

**关键发现:**
- NUMA 本地 vs 远程访问性能差异
- 最佳 CPU 绑定策略
- 内存分配策略影响

---

### 3. tencent_perf_analysis.sh - Perf 深度分析

**功能:**
- ✅ perf stat 统计 (cycles/instructions/cache/branch)
- ✅ perf record 热点采样
- ✅ 火焰图生成 (如果 FlameGraph 可用)
- ✅ 缓存性能分析 (L1/LLC)
- ✅ 分支预测分析
- ✅ CPU 周期和 IPC 分析
- ✅ 热点函数识别

**输出文件:**
```
benchmark-results/tencent-perf-YYYYMMDD-HHMMSS/
├── perf_stat_4p.txt          # 4 生产者统计
├── perf_stat_8p.txt          # 8 生产者统计
├── perf_stat_16p.txt         # 16 生产者统计
├── perf_4p.data              # perf 采样数据
├── perf_report_4p.txt        # perf 报告
├── perf_4p_flamegraph.svg    # 火焰图 (SVG)
├── cache_analysis.txt        # 缓存分析
├── branch_analysis.txt       # 分支预测分析
├── cpu_cycle_analysis.txt    # CPU 周期分析
├── hotspot_summary.txt       # 热点函数汇总
├── perf_summary.txt          # 综合报告
└── perf_analysis.log         # 完整日志
```

**关键指标:**
- **IPC (Instructions Per Cycle):** 理想值 > 1.5
- **缓存未命中率:** L1 < 5%, LLC < 10%
- **分支预测失败率:** < 5%
- **热点函数占比:** 识别 > 5% 的函数

**火焰图使用:**
```bash
# 安装 FlameGraph (可选)
git clone https://github.com/brendangregg/FlameGraph /opt/FlameGraph
export PATH=$PATH:/opt/FlameGraph

# 查看火焰图
firefox benchmark-results/tencent-perf-*/perf_*_flamegraph.svg
```

---

## 📈 结果分析

### 使用 Python 脚本分析

```bash
# 查看正式 Galay / Boost.Asio UDP 交替压测原始输出
cat benchmark-results/tencent-full-*/boost_asio_coro_comparison.txt

# 查看正式 Galay / Boost.Asio TCP 交替压测原始输出
cat benchmark-results/tencent-full-*/boost_asio_coro_tcp_comparison.txt
```

### 手动分析 JSONL 文件

```bash
# 查看有效测试
cat standard_tests.jsonl | jq 'select(.valid == true)'

# 计算平均吞吐量
cat standard_tests.jsonl | \
    jq -r 'select(.valid == true) | .messages_per_second' | \
    awk '{sum+=$1; count++} END {print sum/count/1e6 " M/s"}'

# 按生产者数量分组
cat standard_tests.jsonl | \
    jq -r 'select(.valid == true) | "\(.producers)P: \(.messages_per_second/1e6) M/s"'
```

### 正式外部对标

```bash
# 每个 galay 行后紧跟同轮 boost.asio 输出，避免固定顺序偏差
rg '^(galay|boost[.]asio),|^measured |^status=' \
  benchmark-results/tencent-full-*/boost_asio_coro_comparison.txt
rg '^(galay|boost[.]asio),|^measured |^status=' \
  benchmark-results/tencent-full-*/boost_asio_coro_tcp_comparison.txt
```

---

## 🔧 故障排查

### 问题: perf 权限错误

```bash
# 症状
perf: Permission denied

# 解决方案
sudo sysctl kernel.perf_event_paranoid=-1
# 或临时修改
echo -1 | sudo tee /proc/sys/kernel/perf_event_paranoid
```

### 问题: NUMA 工具不可用

```bash
# 症状
numactl: command not found

# 解决方案
sudo yum install numactl      # CentOS/RHEL
sudo apt-get install numactl  # Ubuntu/Debian
```

### 问题: 二进制文件不存在

```bash
# 症状
错误: 二进制文件不存在: /path/to/build/benchmark/...

# 解决方案
# 确保使用正确的构建类型
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)

# 检查目标是否构建成功
ls -lh build/benchmark/cpp/kernel/benchmark_*
```

### 问题: 测试运行时间过长

```bash
# 修改脚本中的参数
# 编辑脚本,减少测试次数或消息数量

# tencent_full_test.sh
MESSAGES=1000000         # 默认 10000000
RUNS_PER_CONFIG=3        # 默认 5

# tencent_numa_test.sh
MESSAGES=1000000         # 默认 5000000
RUNS_PER_CONFIG=3        # 默认 5

# tencent_perf_analysis.sh
MESSAGES=1000000         # 默认 10000000
DURATION=15              # 默认 30 秒
```

---

## 📝 最佳实践

### 测试环境准备

```bash
# 1. 最小化系统干扰
# 关闭不必要的服务
sudo systemctl stop crond
sudo systemctl stop rsyslog

# 2. 固定 CPU 频率 (避免动态调频)
sudo cpupower frequency-set -g performance

# 3. 禁用透明大页 (可选)
echo never | sudo tee /sys/kernel/mm/transparent_hugepage/enabled

# 4. 增加文件描述符限制
ulimit -n 65535
```

### 推荐测试顺序

```bash
# 第一步: 完整测试,建立基线
./scripts/tencent_full_test.sh

# 第二步: NUMA 优化
./scripts/tencent_numa_test.sh
# 识别最佳 NUMA 配置

# 第三步: 性能剖析
./scripts/tencent_perf_analysis.sh
# 识别热点和瓶颈

# 第四步: 基于发现进行优化
# ... 代码优化 ...

# 第五步: 重新运行测试验证改进
./scripts/tencent_full_test.sh
```

### 结果归档

```bash
# 为结果目录添加描述性标签
mv benchmark-results/tencent-full-20260804-123456 \
   benchmark-results/tencent-full-baseline-20260804

# 对比优化前后
diff <(cat baseline/summary.txt) <(cat optimized/summary.txt)
```

---

## 🎯 性能目标

### 预期指标 (参考)

| 配置 | 目标吞吐量 | 备注 |
|------|-----------|------|
| 1P | 20-40 M/s | 单生产者基线 |
| 4P | 60-100 M/s | 中等并发 |
| 8P | 100-150 M/s | 高并发 |
| 16P | 120-180 M/s | NUMA 影响显现 |
| 32P+ | 待测试 | 高度依赖 NUMA |

### Boost.Asio 协程对标

- **唯一正式对手:** Boost.Asio `co_spawn` / `awaitable`
- **固定口径:** loopback UDP/TCP echo、100 clients、4 workers、256B、单请求在途
- **CPU 亲和性:** 双方默认固定到 CPU 0；可用 `BENCHMARK_CPU=N` 选择同一可用 CPU
- **结论要求:** 至少 3 轮交替运行，报告中位数、丢包和错误，不挑单轮峰值

---

## 📚 扩展阅读

### Perf 工具

- [Perf Wiki](https://perf.wiki.kernel.org/)
- [Brendan Gregg's Perf Examples](http://www.brendangregg.com/perf.html)
- [Flame Graphs](http://www.brendangregg.com/flamegraphs.html)

### NUMA 优化

- [NUMA Best Practices](https://documentation.suse.com/sles/15-SP1/html/SLES-all/cha-tuning-numactl.html)
- [Linux NUMA Memory Policy](https://www.kernel.org/doc/html/latest/admin-guide/mm/numa_memory_policy.html)

### 并发性能

- [Boost.Asio](https://www.boost.org/doc/libs/release/doc/html/boost_asio.html)
- [Lock-Free Programming](https://preshing.com/20120612/an-introduction-to-lock-free-programming/)

---

## 💡 贡献

如果发现脚本问题或有改进建议:

1. 检查日志文件了解详细错误
2. 提交 issue 并附上相关日志片段
3. 提交 PR 改进脚本

---

## 📄 许可

与主项目相同。

---

**最后更新:** 2026-08-04
