#!/bin/bash
# 腾讯机器测试套件 - 快速参考卡

cat << 'EOF'
╔════════════════════════════════════════════════════════════════╗
║           腾讯机器性能测试套件 - 快速参考                        ║
╚════════════════════════════════════════════════════════════════╝

📦 三个核心脚本:
  1️⃣  tencent_full_test.sh      - 完整性能测试 (30-60分钟)
  2️⃣  tencent_numa_test.sh      - NUMA感知测试 (20-40分钟)
  3️⃣  tencent_perf_analysis.sh  - perf深度分析 (15-30分钟)

🚀 快速开始:
  # 1. 构建项目
  cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j

  # 2. 运行完整测试
  ./scripts/tencent_full_test.sh

  # 3. 查看结果
  cat benchmark-results/tencent-full-*/summary.txt

📊 测试内容:

  tencent_full_test.sh
  ├─ 系统信息收集 (CPU/内存/NUMA拓扑)
  ├─ 标准性能测试 (1P-64P, 多容量配置)
  ├─ 内部策略验证 (fair/balanced/throughput)
  ├─ Boost.Asio 协程 UDP 公平对标 (Galay vs Boost.Asio)
  ├─ Boost.Asio 协程 TCP 公平对标 (Galay vs Boost.Asio)
  └─ 自动生成汇总报告

  tencent_numa_test.sh
  ├─ NUMA拓扑检测
  ├─ 同节点 vs 跨节点测试
  ├─ CPU亲和性测试
  └─ 内存分配策略对比

  tencent_perf_analysis.sh
  ├─ perf stat统计
  ├─ perf record采样
  ├─ 火焰图生成 (SVG)
  ├─ 缓存性能分析
  ├─ 分支预测分析
  └─ 热点函数识别

🔧 依赖工具 (可选):
  sudo yum install numactl perf           # CentOS/RHEL
  sudo apt-get install numactl linux-tools-common  # Ubuntu

  # perf权限调整
  sudo sysctl kernel.perf_event_paranoid=-1

  # FlameGraph (火焰图)
  git clone https://github.com/brendangregg/FlameGraph /opt/FlameGraph

📈 输出文件:
  benchmark-results/tencent-*/
  ├─ *.jsonl        - 测试原始数据 (JSON Lines格式)
  ├─ *_summary.txt  - 汇总报告
  ├─ *.log          - 详细日志
  ├─ *.svg          - 火焰图 (仅perf分析)
  └─ system_info.txt - 系统信息

🎯 性能目标 (参考):
  1P:  20-40  M/s  (单生产者基线)
  4P:  60-100 M/s  (中等并发)
  8P:  100-150 M/s (高并发)
  16P: 120-180 M/s (NUMA影响显现)

💡 最佳实践:
  1. 最小化系统干扰 (关闭不必要服务)
  2. 固定CPU频率: sudo cpupower frequency-set -g performance
  3. 按顺序运行: full → numa → perf
  4. 对比优化前后结果

🔍 结果分析:
  # 查看有效测试
  cat results.jsonl | jq 'select(.valid == true)'

  # 计算平均吞吐量
  cat results.jsonl | jq -r '.messages_per_second' | \
    awk '{sum+=$1;n++} END {print sum/n/1e6 " M/s"}'

  # 按生产者分组
  cat results.jsonl | jq -r '"\(.producers)P: \(.messages_per_second/1e6) M/s"'

📚 详细文档:
  scripts/README_TENCENT_TESTS.md

🐛 故障排查:
  - perf权限错误 → sudo sysctl kernel.perf_event_paranoid=-1
  - numactl未找到 → sudo yum install numactl
  - 二进制不存在 → cmake --build build -j
  - 运行时间过长 → 编辑脚本减少MESSAGES和RUNS_PER_CONFIG

EOF
