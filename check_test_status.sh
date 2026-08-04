#!/bin/bash
# 测试进度检查脚本

echo "=== MPSC 策略对比测试 - 进度检查 ==="
echo ""

# 检查进程状态
if ps aux | grep -v grep | grep benchmark_kernel_mpsc_strategy_comparison > /dev/null; then
    echo "✓ 测试进程正在运行"

    # 显示 CPU 使用率
    CPU_USAGE=$(ps aux | grep benchmark_kernel_mpsc_strategy_comparison | grep -v grep | awk '{print $3}')
    echo "  CPU 使用率: ${CPU_USAGE}%"

    # 运行时间
    ELAPSED=$(ps -o etime= -p $(pgrep -f benchmark_kernel_mpsc_strategy_comparison) | tr -d ' ')
    echo "  已运行时间: ${ELAPSED}"

    echo ""
    echo "预计还需 2-5 分钟..."
    echo ""
    echo "你可以："
    echo "  1. 等待完成（推荐）"
    echo "  2. tail -f /tmp/strategy_comparison.log（实时查看）"
    echo "  3. 审查代码和文档"
else
    echo "✗ 测试进程已完成或未运行"
    echo ""

    # 检查结果文件
    if [ -f /tmp/strategy_comparison.log ]; then
        echo "✓ 找到结果文件: /tmp/strategy_comparison.log"
        echo ""
        echo "=== 测试结果摘要 ==="
        echo ""

        # 提取关键结果
        grep -E "测试|P1C|Speedup|✅|⚠️" /tmp/strategy_comparison.log | head -30

        echo ""
        echo "=== 下一步 ==="
        echo "1. 查看完整结果: cat /tmp/strategy_comparison.log"
        echo "2. 运行 Crossbeam 对比: ./benchmark/cpp/kernel/compare/run_crossbeam_comparison.sh"
        echo "3. 分析性能数据并调优"
    else
        echo "✗ 未找到结果文件"
        echo ""
        echo "请重新运行测试:"
        echo "  cd build"
        echo "  ./benchmark/cpp/kernel/benchmark_kernel_mpsc_strategy_comparison"
    fi
fi

echo ""
echo "=== 项目状态 ==="
echo ""
echo "已完成:"
echo "  ✅ 延迟优先策略（指数退避）- 4P: 6.9M → 171.5M/s (24.9x)"
echo "  ✅ 吞吐优先策略（per-producer ring）- 完整实现"
echo "  ✅ 对比基准测试 - 编译通过"
echo "  ✅ 完整文档体系 - 5 篇文档，15,000+ 字"
echo ""
echo "进行中:"
echo "  🔄 本机性能测试（当前）"
echo ""
echo "待完成:"
echo "  📅 Rust Crossbeam 对比（明天）"
echo "  🗓️ 腾讯机器验证（本周）"
echo ""
