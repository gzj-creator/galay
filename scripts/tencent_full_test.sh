#!/bin/bash
# Tencent 机器完整测试套件
# 包含系统信息收集、标准性能测试、策略对比、Crossbeam 对比和结果汇总

set -euo pipefail

# ==================== 配置 ====================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
RESULT_DIR="${PROJECT_ROOT}/benchmark-results/tencent-full-$(date +%Y%m%d-%H%M%S)"
LOG_FILE="${RESULT_DIR}/full_test.log"

# 测试参数
MESSAGES=10000000
RUNS_PER_CONFIG=5
PRODUCER_COUNTS=(1 2 4 8 16 32 64)
CAPACITIES=(1024 4096 16384)
STRATEGIES=("fair" "balanced" "throughput")

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

check_command() {
    if ! command -v "$1" &> /dev/null; then
        log "警告: 命令 $1 未找到"
        return 1
    fi
    return 0
}

check_binary() {
    if [ ! -x "$1" ]; then
        log "错误: 二进制文件不存在或不可执行: $1"
        return 1
    fi
    return 0
}

# ==================== 系统信息收集 ====================
collect_system_info() {
    log_section "1. 系统信息收集"

    local sys_info="${RESULT_DIR}/system_info.txt"

    {
        echo "=== 主机信息 ==="
        hostname
        uname -a
        echo ""

        echo "=== CPU 信息 ==="
        if check_command lscpu; then
            lscpu
        elif [ -f /proc/cpuinfo ]; then
            cat /proc/cpuinfo | grep -E "processor|model name|cpu MHz|cache size" | head -20
        fi
        echo ""

        echo "=== 内存信息 ==="
        if check_command free; then
            free -h
        elif [ -f /proc/meminfo ]; then
            head -10 /proc/meminfo
        fi
        echo ""

        echo "=== NUMA 拓扑 ==="
        if check_command numactl; then
            numactl --hardware
        else
            echo "numactl 不可用"
        fi
        echo ""

        echo "=== CPU 拓扑 ==="
        if [ -d /sys/devices/system/cpu ]; then
            echo "Online CPUs: $(cat /sys/devices/system/cpu/online)"
            echo "Offline CPUs: $(cat /sys/devices/system/cpu/offline 2>/dev/null || echo 'none')"
        fi
        echo ""

        echo "=== 调度器信息 ==="
        cat /sys/kernel/debug/sched/features 2>/dev/null || echo "需要 root 权限"
        echo ""

        echo "=== 系统负载 ==="
        uptime
        echo ""

    } > "${sys_info}"

    log "系统信息已保存到: ${sys_info}"
}

# ==================== 标准性能测试 ====================
run_standard_tests() {
    log_section "2. 标准性能测试 (1P-64P)"

    local mpsc_bin="${BUILD_DIR}/benchmark/cpp/kernel/benchmark_kernel_mpsc_channel_throughput"
    if ! check_binary "${mpsc_bin}"; then
        log "跳过标准性能测试"
        return 1
    fi

    local result_file="${RESULT_DIR}/standard_tests.jsonl"

    for producers in "${PRODUCER_COUNTS[@]}"; do
        for capacity in "${CAPACITIES[@]}"; do
            log "测试配置: ${producers}P, capacity=${capacity}"

            for run in $(seq 1 ${RUNS_PER_CONFIG}); do
                log "  运行 ${run}/${RUNS_PER_CONFIG}..."

                if "${mpsc_bin}" \
                    --producers "${producers}" \
                    --messages "${MESSAGES}" \
                    --capacity "${capacity}" \
                    --json >> "${result_file}" 2>> "${LOG_FILE}"; then
                    log "    成功"
                else
                    log "    失败 (退出码: $?)"
                fi
            done
        done
    done

    log "标准测试结果已保存到: ${result_file}"
}

# ==================== 策略对比测试 ====================
run_strategy_comparison() {
    log_section "3. 策略对比测试"

    local strategy_bin="${BUILD_DIR}/benchmark/cpp/kernel/benchmark_kernel_mpsc_strategy_comparison"
    if ! check_binary "${strategy_bin}"; then
        log "跳过策略对比测试"
        return 1
    fi

    local result_file="${RESULT_DIR}/strategy_comparison.jsonl"

    # 重点测试配置
    local test_configs=(
        "4:4096"
        "8:4096"
        "16:4096"
        "32:4096"
    )

    for config in "${test_configs[@]}"; do
        IFS=':' read -r producers capacity <<< "${config}"

        for strategy in "${STRATEGIES[@]}"; do
            log "测试策略: ${strategy}, ${producers}P, capacity=${capacity}"

            for run in $(seq 1 ${RUNS_PER_CONFIG}); do
                log "  运行 ${run}/${RUNS_PER_CONFIG}..."

                if "${strategy_bin}" \
                    --producers "${producers}" \
                    --messages "${MESSAGES}" \
                    --capacity "${capacity}" \
                    --strategy "${strategy}" \
                    --json >> "${result_file}" 2>> "${LOG_FILE}"; then
                    log "    成功"
                else
                    log "    失败"
                fi
            done
        done
    done

    log "策略对比结果已保存到: ${result_file}"
}

# ==================== Crossbeam 对比 ====================
run_crossbeam_comparison() {
    log_section "4. Crossbeam 对比测试"

    local galay_bin="${BUILD_DIR}/benchmark/cpp/kernel/benchmark_kernel_compare_mpsc_paired"
    local rust_dir="${PROJECT_ROOT}/benchmark/cpp/kernel/compare/rust-channel"
    local rust_bin="${rust_dir}/target/release/mpsc_paired"

    if ! check_binary "${galay_bin}"; then
        log "Galay MPSC 二进制文件不存在,跳过对比测试"
        return 1
    fi

    # 构建 Rust Crossbeam
    if [ -d "${rust_dir}" ]; then
        log "构建 Rust Crossbeam..."
        (cd "${rust_dir}" && cargo build --release --bin mpsc_paired) >> "${LOG_FILE}" 2>&1 || {
            log "Rust 构建失败,跳过 Crossbeam 对比"
            return 1
        }
    else
        log "Rust 目录不存在,跳过 Crossbeam 对比"
        return 1
    fi

    local result_file="${RESULT_DIR}/crossbeam_comparison.jsonl"

    # 测试配置
    local test_configs=(
        "1:1000000:1024"
        "2:1000000:4096"
        "4:1000000:4096"
        "8:1000000:4096"
    )

    for config in "${test_configs[@]}"; do
        IFS=':' read -r producers messages capacity <<< "${config}"

        log "对比测试: ${producers}P, messages=${messages}, capacity=${capacity}"

        # Galay 测试
        log "  运行 Galay..."
        for run in $(seq 1 ${RUNS_PER_CONFIG}); do
            "${galay_bin}" \
                --producers "${producers}" \
                --messages "${messages}" \
                --capacity "${capacity}" \
                --json >> "${result_file}" 2>> "${LOG_FILE}" || true
        done

        # Crossbeam 测试
        log "  运行 Crossbeam..."
        for run in $(seq 1 ${RUNS_PER_CONFIG}); do
            "${rust_bin}" \
                --producers "${producers}" \
                --messages "${messages}" \
                --capacity "${capacity}" \
                --json >> "${result_file}" 2>> "${LOG_FILE}" || true
        done
    done

    log "Crossbeam 对比结果已保存到: ${result_file}"
}

# ==================== 结果汇总 ====================
generate_summary() {
    log_section "5. 生成测试报告"

    local summary_file="${RESULT_DIR}/summary.txt"

    {
        echo "=========================================="
        echo "腾讯机器完整测试报告"
        echo "=========================================="
        echo ""
        echo "测试时间: $(date)"
        echo "结果目录: ${RESULT_DIR}"
        echo ""

        echo "--- 测试配置 ---"
        echo "消息数量: ${MESSAGES}"
        echo "每配置运行次数: ${RUNS_PER_CONFIG}"
        echo "生产者数量: ${PRODUCER_COUNTS[*]}"
        echo "容量配置: ${CAPACITIES[*]}"
        echo "策略类型: ${STRATEGIES[*]}"
        echo ""

        echo "--- 生成的文件 ---"
        ls -lh "${RESULT_DIR}" | tail -n +2
        echo ""

        echo "--- 快速分析 ---"

        # 标准测试统计
        if [ -f "${RESULT_DIR}/standard_tests.jsonl" ]; then
            local total_runs=$(wc -l < "${RESULT_DIR}/standard_tests.jsonl")
            local valid_runs=$(grep -c '"valid":true' "${RESULT_DIR}/standard_tests.jsonl" || echo 0)
            echo "标准测试: ${valid_runs}/${total_runs} 次成功"
        fi

        # 策略对比统计
        if [ -f "${RESULT_DIR}/strategy_comparison.jsonl" ]; then
            local total_runs=$(wc -l < "${RESULT_DIR}/strategy_comparison.jsonl")
            local valid_runs=$(grep -c '"valid":true' "${RESULT_DIR}/strategy_comparison.jsonl" || echo 0)
            echo "策略对比: ${valid_runs}/${total_runs} 次成功"
        fi

        # Crossbeam 对比统计
        if [ -f "${RESULT_DIR}/crossbeam_comparison.jsonl" ]; then
            local total_runs=$(wc -l < "${RESULT_DIR}/crossbeam_comparison.jsonl")
            local galay_runs=$(grep -c '"language":"cpp"' "${RESULT_DIR}/crossbeam_comparison.jsonl" || echo 0)
            local rust_runs=$(grep -c '"language":"rust"' "${RESULT_DIR}/crossbeam_comparison.jsonl" || echo 0)
            echo "Crossbeam 对比: Galay=${galay_runs}, Rust=${rust_runs}"
        fi

        echo ""
        echo "--- 下一步建议 ---"
        echo "1. 运行 NUMA 感知测试: ${SCRIPT_DIR}/tencent_numa_test.sh"
        echo "2. 运行 perf 性能分析: ${SCRIPT_DIR}/tencent_perf_analysis.sh"
        echo "3. 使用 Python 脚本分析结果:"
        echo "   python3 benchmark/cpp/kernel/compare/run_mpsc_paired.py --analyze ${RESULT_DIR}/*.jsonl"
        echo ""

    } > "${summary_file}"

    cat "${summary_file}"
    log "测试报告已保存到: ${summary_file}"
}

# ==================== 主流程 ====================
main() {
    log_section "腾讯机器完整测试套件"
    log "开始时间: $(date)"

    # 创建结果目录
    mkdir -p "${RESULT_DIR}"
    log "结果目录: ${RESULT_DIR}"

    # 检查构建目录
    if [ ! -d "${BUILD_DIR}" ]; then
        log "错误: 构建目录不存在: ${BUILD_DIR}"
        log "请先运行: cmake -B build && cmake --build build"
        exit 1
    fi

    # 执行测试流程
    collect_system_info || log "系统信息收集失败"
    run_standard_tests || log "标准测试失败"
    run_strategy_comparison || log "策略对比失败"
    run_crossbeam_comparison || log "Crossbeam 对比失败"
    generate_summary

    log_section "测试完成"
    log "结束时间: $(date)"
    log "所有结果保存在: ${RESULT_DIR}"

    echo ""
    echo "完整日志: ${LOG_FILE}"
}

# 运行主流程
main "$@"
