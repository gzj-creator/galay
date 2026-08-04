#!/bin/bash
# 腾讯机器部署和测试脚本
# 用途：在腾讯生产环境验证 MPSC 双策略性能

set -e

# ============================================
# 配置区
# ============================================

# 腾讯机器信息（请根据实际情况修改）
TENCENT_HOST="your-tencent-machine"
TENCENT_USER="your-username"
TENCENT_PORT="22"
REMOTE_BASE_DIR="/home/${TENCENT_USER}/galay-test"

# 本地项目路径
LOCAL_PROJECT="/Users/gongzhijie/Desktop/projects/git/galay"

# 测试配置
TEST_ROUNDS=10
MAX_PRODUCERS=32  # 腾讯机器核心数更多，测试更高并发

# ============================================
# 函数定义
# ============================================

log() {
    echo "[$(date +'%Y-%m-%d %H:%M:%S')] $*"
}

error() {
    echo "[ERROR] $*" >&2
    exit 1
}

# SSH 执行命令
remote_exec() {
    ssh -p ${TENCENT_PORT} ${TENCENT_USER}@${TENCENT_HOST} "$@"
}

# SCP 上传文件
remote_upload() {
    local src="$1"
    local dst="$2"
    scp -P ${TENCENT_PORT} -r "$src" "${TENCENT_USER}@${TENCENT_HOST}:${dst}"
}

# SCP 下载文件
remote_download() {
    local src="$1"
    local dst="$2"
    scp -P ${TENCENT_PORT} "${TENCENT_USER}@${TENCENT_HOST}:${src}" "$dst"
}

# ============================================
# 部署流程
# ============================================

deploy() {
    log "=== 开始部署到腾讯机器 ==="

    # 1. 创建远程目录
    log "1. 创建远程目录..."
    remote_exec "mkdir -p ${REMOTE_BASE_DIR}"

    # 2. 上传源代码
    log "2. 上传源代码..."
    cd "${LOCAL_PROJECT}"

    # 打包需要的文件（排除 build 目录）
    tar czf /tmp/galay-mpsc.tar.gz \
        --exclude='build' \
        --exclude='.git' \
        --exclude='benchmark-results' \
        src/ \
        benchmark/ \
        CMakeLists.txt \
        cmake/ \
        docs/optimization/

    remote_upload "/tmp/galay-mpsc.tar.gz" "${REMOTE_BASE_DIR}/"
    rm /tmp/galay-mpsc.tar.gz

    # 3. 解压
    log "3. 解压代码..."
    remote_exec "cd ${REMOTE_BASE_DIR} && tar xzf galay-mpsc.tar.gz"

    log "✓ 部署完成"
}

# ============================================
# 编译流程
# ============================================

build() {
    log "=== 开始编译 ==="

    # 检查依赖
    log "1. 检查编译依赖..."
    remote_exec "which cmake g++ make" || error "缺少编译工具"

    # 配置 CMake
    log "2. 配置 CMake (Release 模式)..."
    remote_exec "cd ${REMOTE_BASE_DIR} && cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS='-march=native -O3'"

    # 编译
    log "3. 编译项目..."
    remote_exec "cd ${REMOTE_BASE_DIR} && cmake --build build -j\$(nproc)"

    log "✓ 编译完成"
}

# ============================================
# 测试流程
# ============================================

test_performance() {
    log "=== 开始性能测试 ==="

    # 1. 获取系统信息
    log "1. 收集系统信息..."
    remote_exec "
        echo '=== CPU 信息 ==='
        lscpu | grep -E 'Model name|Thread|Core|Socket|NUMA'
        echo ''
        echo '=== 缓存信息 ==='
        lscpu | grep -E 'L1|L2|L3'
        echo ''
        echo '=== 内存信息 ==='
        free -h
    " > /tmp/tencent_system_info.txt

    cat /tmp/tencent_system_info.txt

    # 2. 运行策略对比测试
    log "2. 运行策略对比测试..."
    remote_exec "cd ${REMOTE_BASE_DIR}/build && ./benchmark/cpp/kernel/benchmark_kernel_mpsc_strategy_comparison" \
        > /tmp/tencent_strategy_comparison.log

    # 3. 运行原有的 bounded channel 测试
    log "3. 运行 bounded channel 基准测试..."
    remote_exec "cd ${REMOTE_BASE_DIR}/build && ./benchmark/cpp/kernel/benchmark_kernel_bounded_channel_throughput" \
        > /tmp/tencent_bounded_throughput.log

    # 4. 下载结果
    log "4. 下载测试结果..."
    mkdir -p benchmark-results/tencent-$(date +%Y%m%d)
    cp /tmp/tencent_*.log benchmark-results/tencent-$(date +%Y%m%d)/

    log "✓ 测试完成，结果保存在 benchmark-results/tencent-$(date +%Y%m%d)/"
}

# ============================================
# 性能分析
# ============================================

analyze_performance() {
    log "=== 开始性能分析 ==="

    # 使用 perf 分析
    log "1. 运行 perf 分析..."
    remote_exec "
        cd ${REMOTE_BASE_DIR}/build

        # perf stat 统计
        perf stat -e cache-references,cache-misses,L1-dcache-loads,L1-dcache-load-misses,cycles,instructions \
            ./benchmark/cpp/kernel/benchmark_kernel_mpsc_strategy_comparison \
            2>&1 | tee perf_stat.log

        # perf record + report
        perf record -g ./benchmark/cpp/kernel/benchmark_kernel_mpsc_strategy_comparison
        perf report --stdio > perf_report.txt
    " || log "警告：perf 分析失败（可能需要 sudo 权限）"

    # 下载分析结果
    remote_download "${REMOTE_BASE_DIR}/build/perf_*.log" benchmark-results/tencent-$(date +%Y%m%d)/ || true
    remote_download "${REMOTE_BASE_DIR}/build/perf_*.txt" benchmark-results/tencent-$(date +%Y%m%d)/ || true

    log "✓ 性能分析完成"
}

# ============================================
# Rust Crossbeam 对比
# ============================================

test_crossbeam() {
    log "=== Rust Crossbeam 对比测试 ==="

    # 检查 Rust 环境
    remote_exec "which cargo" || {
        log "Rust 未安装，跳过 Crossbeam 对比"
        return
    }

    # 运行 Rust 测试
    log "运行 Crossbeam 基准测试..."
    remote_exec "
        cd ${REMOTE_BASE_DIR}/benchmark/cpp/kernel/compare/rust-channel
        cargo bench 2>&1 | tee crossbeam_bench.log
    " > /tmp/tencent_crossbeam.log

    # 下载结果
    cp /tmp/tencent_crossbeam.log benchmark-results/tencent-$(date +%Y%m%d)/

    log "✓ Crossbeam 对比完成"
}

# ============================================
# 压力测试
# ============================================

stress_test() {
    log "=== 压力测试 (24小时) ==="

    log "启动 24 小时压力测试（后台运行）..."
    remote_exec "
        cd ${REMOTE_BASE_DIR}/build
        nohup bash -c '
            for i in {1..1000}; do
                echo \"[Iteration \$i] \$(date)\"
                ./benchmark/cpp/kernel/benchmark_kernel_mpsc_strategy_comparison
                sleep 60
            done
        ' > stress_test.log 2>&1 &

        echo \"压力测试已启动（PID: \$!）\"
        echo \"监控日志: tail -f ${REMOTE_BASE_DIR}/build/stress_test.log\"
    "

    log "✓ 压力测试已启动（后台运行）"
    log "   监控命令: ssh ${TENCENT_USER}@${TENCENT_HOST} 'tail -f ${REMOTE_BASE_DIR}/build/stress_test.log'"
}

# ============================================
# 生成报告
# ============================================

generate_report() {
    log "=== 生成测试报告 ==="

    local report_dir="benchmark-results/tencent-$(date +%Y%m%d)"
    local report_file="${report_dir}/TENCENT_PERFORMANCE_REPORT.md"

    cat > "${report_file}" <<EOF
# 腾讯机器性能测试报告

生成时间：$(date)

## 1. 测试环境

$(cat ${report_dir}/tencent_system_info.txt 2>/dev/null || echo "系统信息未收集")

## 2. 策略对比测试

\`\`\`
$(cat ${report_dir}/tencent_strategy_comparison.log 2>/dev/null || echo "测试结果未收集")
\`\`\`

## 3. Bounded Channel 基准测试

\`\`\`
$(cat ${report_dir}/tencent_bounded_throughput.log 2>/dev/null || echo "测试结果未收集")
\`\`\`

## 4. Crossbeam 对比

\`\`\`
$(cat ${report_dir}/tencent_crossbeam.log 2>/dev/null || echo "Crossbeam 测试未运行")
\`\`\`

## 5. 性能分析

\`\`\`
$(cat ${report_dir}/perf_stat.log 2>/dev/null || echo "性能分析未运行")
\`\`\`

## 6. 结论

（待补充）

## 7. 建议

（待补充）

---

**测试完成时间**：$(date)
EOF

    log "✓ 报告已生成: ${report_file}"
}

# ============================================
# 清理
# ============================================

cleanup() {
    log "=== 清理远程环境 ==="

    read -p "是否删除远程测试目录？(y/n) " -n 1 -r
    echo
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        remote_exec "rm -rf ${REMOTE_BASE_DIR}"
        log "✓ 已删除远程目录"
    else
        log "保留远程目录: ${REMOTE_BASE_DIR}"
    fi
}

# ============================================
# 主流程
# ============================================

main() {
    log "=== 腾讯机器 MPSC 性能测试流程 ==="
    echo ""
    echo "测试步骤："
    echo "  1. 部署代码到腾讯机器"
    echo "  2. 编译项目 (Release + -march=native)"
    echo "  3. 运行性能测试"
    echo "  4. 运行性能分析 (perf)"
    echo "  5. Crossbeam 对比测试"
    echo "  6. 生成测试报告"
    echo ""

    read -p "确认开始？(y/n) " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        log "已取消"
        exit 0
    fi

    # 执行流程
    deploy
    build
    test_performance
    analyze_performance
    test_crossbeam
    generate_report

    echo ""
    log "=== 所有测试完成 ==="
    echo ""
    echo "测试结果："
    echo "  本地目录: benchmark-results/tencent-$(date +%Y%m%d)/"
    echo "  远程目录: ${TENCENT_USER}@${TENCENT_HOST}:${REMOTE_BASE_DIR}/build/"
    echo ""
    echo "可选操作："
    echo "  - 压力测试: $0 stress"
    echo "  - 清理环境: $0 cleanup"
    echo "  - 重新测试: $0 test"
    echo ""
}

# ============================================
# 命令行接口
# ============================================

case "${1:-full}" in
    deploy)
        deploy
        ;;
    build)
        build
        ;;
    test)
        test_performance
        ;;
    analyze)
        analyze_performance
        ;;
    crossbeam)
        test_crossbeam
        ;;
    stress)
        stress_test
        ;;
    report)
        generate_report
        ;;
    cleanup)
        cleanup
        ;;
    full)
        main
        ;;
    *)
        echo "用法: $0 {deploy|build|test|analyze|crossbeam|stress|report|cleanup|full}"
        echo ""
        echo "命令说明："
        echo "  deploy    - 仅部署代码"
        echo "  build     - 仅编译"
        echo "  test      - 仅运行性能测试"
        echo "  analyze   - 仅运行性能分析"
        echo "  crossbeam - 仅运行 Crossbeam 对比"
        echo "  stress    - 运行 24 小时压力测试"
        echo "  report    - 生成测试报告"
        echo "  cleanup   - 清理远程环境"
        echo "  full      - 完整流程（默认）"
        exit 1
        ;;
esac
