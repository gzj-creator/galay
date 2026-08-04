#!/bin/bash
# 工作流监控面板

clear

cat << 'EOF'
╔══════════════════════════════════════════════════════════════════════╗
║              MPSC 项目 - 多工作流并行执行监控面板                      ║
╚══════════════════════════════════════════════════════════════════════╝
EOF

echo ""
echo "🚀 并行工作流状态"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

# 工作流 1: 完整实施
echo "1️⃣  工作流: mpsc-throughput-complete-implementation"
echo "   任务: 测试验证 → Async 支持 → Rust 对比 → 文档"
echo "   状态: 运行中 🔄"
echo ""

# 工作流 2: 统一 API
echo "2️⃣  工作流: mpsc-unified-api-design"
echo "   任务: API 设计 → 实现 → 测试"
echo "   状态: 运行中 🔄"
echo ""

# 工作流 3: 参数调优
echo "3️⃣  工作流: mpsc-optimization-tuning"
echo "   任务: 参数分析 → 调优实验 → 最优配置"
echo "   状态: 运行中 🔄"
echo ""

# 工作流 4: Code Review
echo "4️⃣  工作流: mpsc-code-review-prep"
echo "   任务: 代码检查 → 测试覆盖 → PR 准备"
echo "   状态: 运行中 🔄"
echo ""

# 工作流 5: 腾讯部署
echo "5️⃣  工作流: mpsc-tencent-deployment-prep"
echo "   任务: 环境检查 → 测试套件 → 报告模板"
echo "   状态: 运行中 🔄"
echo ""

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
echo "📊 性能测试状态"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

# 检查性能测试
if ps aux | grep -v grep | grep benchmark_kernel_mpsc_strategy_comparison > /dev/null; then
    CPU=$(ps aux | grep benchmark_kernel_mpsc_strategy_comparison | grep -v grep | awk '{print $3}' | head -1)
    TIME=$(ps -o etime= -p $(pgrep -f benchmark_kernel_mpsc_strategy_comparison | head -1) 2>/dev/null | tr -d ' ')
    echo "   状态: 运行中 🔄"
    echo "   CPU:  ${CPU}%"
    echo "   时长: ${TIME}"
else
    if [ -f /tmp/strategy_comparison.log ] && [ -s /tmp/strategy_comparison.log ]; then
        echo "   状态: 已完成 ✅"
        echo "   结果: /tmp/strategy_comparison.log"
    else
        echo "   状态: 未运行"
    fi
fi

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
echo "📈 预期产出"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
echo "工作流 1 产出:"
echo "  ✅ 性能测试分析报告"
echo "  ✅ ThroughputBoundedSendAwaitable/RecvAwaitable 实现"
echo "  ✅ Rust Crossbeam 对比报告"
echo "  ✅ 使用指南和最佳实践文档"
echo ""
echo "工作流 2 产出:"
echo "  ✅ channel_factory.h - 统一 API"
echo "  ✅ 自动策略选择机制"
echo "  ✅ API 测试用例"
echo ""
echo "工作流 3 产出:"
echo "  ✅ 参数调优报告"
echo "  ✅ b29_mpsc_parameter_tuning.cc 测试"
echo "  ✅ 最优参数配置"
echo ""
echo "工作流 4 产出:"
echo "  ✅ 代码质量检查报告"
echo "  ✅ 测试覆盖报告"
echo "  ✅ PR 模板和检查清单"
echo ""
echo "工作流 5 产出:"
echo "  ✅ 腾讯部署指南"
echo "  ✅ 完整测试套件脚本"
echo "  ✅ 性能报告模板"
echo ""

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
echo "⏱️  预计完成时间"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
echo "  工作流 1-5: 15-30 分钟（并行执行）"
echo "  性能测试:   5-10 分钟（已运行 20+ 分钟）"
echo ""
echo "  总计: 约 30-40 分钟完成所有任务"
echo ""

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
echo "💡 命令参考"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
echo "  查看工作流详情:  /workflows"
echo "  检查测试状态:    ./check_test_status.sh"
echo "  刷新监控面板:    ./monitor_workflows.sh"
echo "  腾讯机器部署:    ./deploy_to_tencent.sh full"
echo ""

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
echo "当前时间: $(date '+%Y-%m-%d %H:%M:%S')"
echo "状态: 🔄 多工作流并行执行中..."
echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""
