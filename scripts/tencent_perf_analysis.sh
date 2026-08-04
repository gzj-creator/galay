#!/bin/bash
# Tencent 机器 perf 性能分析脚本
# 使用 Linux perf 工具进行深度性能分析

set -euo pipefail

# ==================== 配置 ====================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
RESULT_DIR="${PROJECT_ROOT}/benchmark-results/tencent-perf-$(date +%Y%m%d-%H%M%S)"
LOG_FILE="${RESULT_DIR}/perf_analysis.log"

# 测试参数
MESSAGES=10000000
CAPACITY=4096
DURATION=30  # perf record 持续时间(秒)

# Perf 事件
PERF_EVENTS=(
    "cycles"
    "instructions"
    "cache-references"
    "cache-misses"
    "branch-instructions"
    "branch-misses"
    "L1-dcache-load-misses"
    "LLC-load-misses"
)

# ==================== 工具函数 ====================
log() {
    echo "[$(date +%H:%M:%S)] $*" | tee -a "${LOG_FILE}"
}

log_section() {
    echo "" | tee -a "${LOG_FILE}"
    echo "========================================" | tee -a "${LOG_FILE}"
    echo "$*" | tee -a "${LOG_FILE}"
    echo "========================================" | tee -a "${LOG_FILE}"
}

check_perf_available() {
    if ! command -v perf &> /dev/null; then
        log "错误: perf 命令不可用"
        log "请安装: yum install perf 或 apt-get install linux-tools-common"
        return 1
    fi

    # 检查 perf 权限
    if ! perf stat -e cycles -- sleep 0.1 &> /dev/null; then
        log "警告: perf 可能需要 root 权限或调整 /proc/sys/kernel/perf_event_paranoid"
        log "当前设置: $(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo 'unknown')"
        log "建议设置: sudo sysctl kernel.perf_event_paranoid=-1"
        return 1
    fi

    log "perf 工具可用"
    return 0
}

check_flamegraph_available() {
    if command -v flamegraph.pl &> /dev/null || [ -f /opt/FlameGraph/flamegraph.pl ]; then
        log "FlameGraph 工具可用"
        return 0
    else
        log "FlameGraph 工具不可用 (可选)"
        log "安装方法: git clone https://github.com/brendangregg/FlameGraph /opt/FlameGraph"
        return 1
    fi
}

# ==================== Perf stat 统计分析 ====================
run_perf_stat() {
    log_section "1. Perf Stat 统计分析"

    local mpsc_bin="${BUILD_DIR}/benchmark/cpp/kernel/benchmark_kernel_mpsc_channel_throughput"
    if [ ! -x "${mpsc_bin}" ]; then
        log "错误: 二进制文件不存在: ${mpsc_bin}"
        return 1
    fi

    # 测试不同生产者数量
    for producers in 1 4 8 16; do
        log "分析 ${producers} 生产者配置..."

        local stat_file="${RESULT_DIR}/perf_stat_${producers}p.txt"

        # 构建事件列表
        local events=$(IFS=,; echo "${PERF_EVENTS[*]}")

        log "  运行 perf stat..."
        perf stat -e "${events}" -o "${stat_file}" \
            "${mpsc_bin}" \
            --producers "${producers}" \
            --messages "${MESSAGES}" \
            --capacity "${CAPACITY}" \
            2>&1 | tee -a "${LOG_FILE}" || {
            log "  perf stat 失败"
            continue
        }

        log "  结果已保存到: ${stat_file}"

        # 显示关键指标
        if [ -f "${stat_file}" ]; then
            log "  关键指标:"
            grep -E "cycles|instructions|cache-misses|branch-misses" "${stat_file}" | tee -a "${LOG_FILE}"
        fi
    done
}

# ==================== Perf record 采样分析 ====================
run_perf_record() {
    log_section "2. Perf Record 采样分析"

    local mpsc_bin="${BUILD_DIR}/benchmark/cpp/kernel/benchmark_kernel_mpsc_channel_throughput"
    if [ ! -x "${mpsc_bin}" ]; then
        log "错误: 二进制文件不存在: ${mpsc_bin}"
        return 1
    fi

    # 测试重点配置
    local configs=(
        "4:4096"
        "8:4096"
        "16:4096"
    )

    for config in "${configs[@]}"; do
        IFS=':' read -r producers capacity <<< "${config}"
        log "采样分析: ${producers}P, capacity=${capacity}"

        local perf_data="${RESULT_DIR}/perf_${producers}p.data"
        local report_file="${RESULT_DIR}/perf_report_${producers}p.txt"

        # 后台运行 benchmark
        log "  启动 benchmark..."
        "${mpsc_bin}" \
            --producers "${producers}" \
            --messages "$((MESSAGES * 3))" \
            --capacity "${capacity}" \
            > /dev/null 2>&1 &
        local bench_pid=$!

        # 等待一下让程序启动
        sleep 2

        # 检查进程是否还在运行
        if ! kill -0 "${bench_pid}" 2>/dev/null; then
            log "  benchmark 进程已退出,跳过采样"
            continue
        fi

        # 运行 perf record
        log "  运行 perf record (${DURATION}秒)..."
        perf record -F 99 -p "${bench_pid}" -g -o "${perf_data}" -- sleep "${DURATION}" 2>&1 | tee -a "${LOG_FILE}" || {
            log "  perf record 失败"
            kill "${bench_pid}" 2>/dev/null || true
            continue
        }

        # 等待 benchmark 完成
        wait "${bench_pid}" 2>/dev/null || true

        # 生成报告
        log "  生成 perf report..."
        perf report -i "${perf_data}" --stdio > "${report_file}" 2>&1 || {
            log "  perf report 失败"
            continue
        }

        log "  报告已保存到: ${report_file}"

        # 显示热点函数
        log "  热点函数 Top 10:"
        head -50 "${report_file}" | grep -A 10 "# Overhead" | tee -a "${LOG_FILE}"
    done
}

# ==================== 火焰图生成 ====================
generate_flamegraph() {
    log_section "3. 生成火焰图"

    # 检查 FlameGraph 工具
    local flamegraph_dir=""
    if command -v flamegraph.pl &> /dev/null; then
        flamegraph_dir="$(dirname $(which flamegraph.pl))"
    elif [ -f /opt/FlameGraph/flamegraph.pl ]; then
        flamegraph_dir="/opt/FlameGraph"
    else
        log "FlameGraph 工具不可用,跳过火焰图生成"
        log "安装方法: git clone https://github.com/brendangregg/FlameGraph /opt/FlameGraph"
        return 1
    fi

    log "使用 FlameGraph: ${flamegraph_dir}"

    # 为每个 perf.data 生成火焰图
    for perf_data in "${RESULT_DIR}"/perf_*.data; do
        if [ ! -f "${perf_data}" ]; then
            continue
        fi

        local basename=$(basename "${perf_data}" .data)
        local folded_file="${RESULT_DIR}/${basename}_folded.txt"
        local svg_file="${RESULT_DIR}/${basename}_flamegraph.svg"

        log "生成火焰图: ${basename}"

        # 转换为折叠格式
        log "  转换为折叠格式..."
        perf script -i "${perf_data}" 2>/dev/null | \
            "${flamegraph_dir}/stackcollapse-perf.pl" > "${folded_file}" 2>&1 || {
            log "  stackcollapse 失败"
            continue
        }

        # 生成 SVG
        log "  生成 SVG..."
        "${flamegraph_dir}/flamegraph.pl" "${folded_file}" > "${svg_file}" 2>&1 || {
            log "  flamegraph 生成失败"
            continue
        }

        log "  火焰图已保存: ${svg_file}"
    done
}

# ==================== 缓存分析 ====================
run_cache_analysis() {
    log_section "4. 缓存性能分析"

    local mpsc_bin="${BUILD_DIR}/benchmark/cpp/kernel/benchmark_kernel_mpsc_channel_throughput"
    if [ ! -x "${mpsc_bin}" ]; then
        log "错误: 二进制文件不存在: ${mpsc_bin}"
        return 1
    fi

    local cache_file="${RESULT_DIR}/cache_analysis.txt"

    {
        echo "=========================================="
        echo "缓存性能分析"
        echo "=========================================="
        echo ""

        for producers in 4 8 16; do
            echo "--- ${producers} 生产者 ---"

            # L1 缓存事件
            perf stat -e L1-dcache-loads,L1-dcache-load-misses,L1-dcache-stores \
                "${mpsc_bin}" \
                --producers "${producers}" \
                --messages "${MESSAGES}" \
                --capacity "${CAPACITY}" 2>&1 || true

            echo ""

            # LLC 缓存事件
            perf stat -e LLC-loads,LLC-load-misses,LLC-stores,LLC-store-misses \
                "${mpsc_bin}" \
                --producers "${producers}" \
                --messages "${MESSAGES}" \
                --capacity "${CAPACITY}" 2>&1 || true

            echo ""
            echo "=========================================="
            echo ""
        done

    } > "${cache_file}"

    cat "${cache_file}" | tee -a "${LOG_FILE}"
}

# ==================== 分支预测分析 ====================
run_branch_analysis() {
    log_section "5. 分支预测分析"

    local mpsc_bin="${BUILD_DIR}/benchmark/cpp/kernel/benchmark_kernel_mpsc_channel_throughput"
    if [ ! -x "${mpsc_bin}" ]; then
        log "错误: 二进制文件不存在: ${mpsc_bin}"
        return 1
    fi

    local branch_file="${RESULT_DIR}/branch_analysis.txt"

    {
        echo "=========================================="
        echo "分支预测分析"
        echo "=========================================="
        echo ""

        for producers in 4 8 16; do
            echo "--- ${producers} 生产者 ---"

            perf stat -e branches,branch-misses,branch-loads,branch-load-misses \
                "${mpsc_bin}" \
                --producers "${producers}" \
                --messages "${MESSAGES}" \
                --capacity "${CAPACITY}" 2>&1 || true

            echo ""
            echo "=========================================="
            echo ""
        done

    } > "${branch_file}"

    cat "${branch_file}" | tee -a "${LOG_FILE}"
}

# ==================== CPU 周期分析 ====================
run_cpu_cycle_analysis() {
    log_section "6. CPU 周期详细分析"

    local mpsc_bin="${BUILD_DIR}/benchmark/cpp/kernel/benchmark_kernel_mpsc_channel_throughput"
    if [ ! -x "${mpsc_bin}" ]; then
        log "错误: 二进制文件不存在: ${mpsc_bin}"
        return 1
    fi

    local cycle_file="${RESULT_DIR}/cpu_cycle_analysis.txt"

    {
        echo "=========================================="
        echo "CPU 周期分析"
        echo "=========================================="
        echo ""

        for producers in 4 8 16; do
            echo "--- ${producers} 生产者 ---"

            # 基础周期指标
            perf stat -e cycles,instructions,stalled-cycles-frontend,stalled-cycles-backend \
                "${mpsc_bin}" \
                --producers "${producers}" \
                --messages "${MESSAGES}" \
                --capacity "${CAPACITY}" 2>&1 || true

            echo ""

            # IPC 分析
            echo "计算 IPC (Instructions Per Cycle)..."
            perf stat -e cycles,instructions \
                "${mpsc_bin}" \
                --producers "${producers}" \
                --messages "${MESSAGES}" \
                --capacity "${CAPACITY}" 2>&1 | \
                grep -E "cycles|instructions" || true

            echo ""
            echo "=========================================="
            echo ""
        done

    } > "${cycle_file}"

    cat "${cycle_file}" | tee -a "${LOG_FILE}"
}

# ==================== 热点函数分析 ====================
analyze_hotspots() {
    log_section "7. 热点函数汇总分析"

    local hotspot_file="${RESULT_DIR}/hotspot_summary.txt"

    {
        echo "=========================================="
        echo "热点函数汇总"
        echo "=========================================="
        echo ""

        # 分析所有 perf report
        for report in "${RESULT_DIR}"/perf_report_*.txt; do
            if [ ! -f "${report}" ]; then
                continue
            fi

            local basename=$(basename "${report}")
            echo "--- ${basename} ---"
            echo ""

            # 提取 Top 20 热点函数
            head -100 "${report}" | \
                grep -A 20 "# Overhead" | \
                grep -E "^\s+[0-9]" | \
                head -20

            echo ""
            echo "=========================================="
            echo ""
        done

    } > "${hotspot_file}"

    cat "${hotspot_file}"
}

# ==================== 生成综合报告 ====================
generate_summary() {
    log_section "8. 生成综合性能报告"

    local summary_file="${RESULT_DIR}/perf_summary.txt"

    {
        echo "=========================================="
        echo "Perf 性能分析综合报告"
        echo "=========================================="
        echo ""
        echo "分析时间: $(date)"
        echo "结果目录: ${RESULT_DIR}"
        echo ""

        echo "--- 生成的文件 ---"
        ls -lh "${RESULT_DIR}" | tail -n +2
        echo ""

        echo "--- 关键发现 ---"
        echo ""

        # 缓存命中率分析
        if [ -f "${RESULT_DIR}/cache_analysis.txt" ]; then
            echo "缓存性能:"
            grep -E "L1-dcache|LLC" "${RESULT_DIR}/cache_analysis.txt" | \
                grep -v "^#" | head -10 || echo "  无缓存数据"
            echo ""
        fi

        # 分支预测率
        if [ -f "${RESULT_DIR}/branch_analysis.txt" ]; then
            echo "分支预测:"
            grep -E "branch-misses" "${RESULT_DIR}/branch_analysis.txt" | \
                grep -v "^#" | head -5 || echo "  无分支预测数据"
            echo ""
        fi

        # IPC
        if [ -f "${RESULT_DIR}/cpu_cycle_analysis.txt" ]; then
            echo "IPC (Instructions Per Cycle):"
            grep -A 2 "IPC" "${RESULT_DIR}/cpu_cycle_analysis.txt" || echo "  无 IPC 数据"
            echo ""
        fi

        echo "--- 性能瓶颈识别 ---"
        echo "1. 检查缓存未命中率是否过高 (> 10%)"
        echo "2. 检查分支预测失败率 (> 5% 可能影响性能)"
        echo "3. 检查 IPC 是否过低 (< 1.0 说明执行效率低)"
        echo "4. 查看火焰图识别热点函数"
        echo ""

        echo "--- 可视化结果 ---"
        if ls "${RESULT_DIR}"/*.svg &> /dev/null; then
            echo "火焰图:"
            ls "${RESULT_DIR}"/*.svg
        else
            echo "未生成火焰图"
        fi
        echo ""

        echo "--- 优化建议 ---"
        echo "1. 针对热点函数进行优化"
        echo "2. 减少缓存未命中 (数据局部性、预取)"
        echo "3. 优化分支预测 (likely/unlikely 注解)"
        echo "4. 考虑 NUMA 亲和性优化"
        echo "5. 评估无锁算法的改进空间"
        echo ""

        echo "--- 下一步行动 ---"
        echo "1. 基于热点函数进行针对性优化"
        echo "2. 在 NUMA 优化配置下重新测试"
        echo "3. 对比优化前后的 perf 数据"
        echo "4. 使用 perf annotate 查看汇编级热点"
        echo ""

    } > "${summary_file}"

    cat "${summary_file}"
}

# ==================== 主流程 ====================
main() {
    log_section "Perf 性能深度分析"
    log "开始时间: $(date)"

    # 创建结果目录
    mkdir -p "${RESULT_DIR}"
    log "结果目录: ${RESULT_DIR}"

    # 检查 perf 可用性
    if ! check_perf_available; then
        log "perf 不可用,尝试继续..."
    fi

    # 检查 FlameGraph
    check_flamegraph_available || true

    # 执行分析
    run_perf_stat || log "perf stat 失败"
    run_perf_record || log "perf record 失败"
    generate_flamegraph || log "火焰图生成失败"
    run_cache_analysis || log "缓存分析失败"
    run_branch_analysis || log "分支分析失败"
    run_cpu_cycle_analysis || log "CPU 周期分析失败"
    analyze_hotspots || log "热点分析失败"
    generate_summary

    log_section "分析完成"
    log "结束时间: $(date)"
    log "所有结果保存在: ${RESULT_DIR}"

    echo ""
    echo "完整日志: ${LOG_FILE}"
    echo ""
    echo "查看火焰图:"
    echo "  在浏览器中打开: ${RESULT_DIR}/*.svg"
}

# 运行主流程
main "$@"
