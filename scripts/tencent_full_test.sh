#!/bin/bash
# Tencent 机器完整测试套件
# 包含系统信息收集、标准性能测试、内部策略验证、Boost.Asio 协程对标和结果汇总

set -euo pipefail

# ==================== 配置 ====================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-${PROJECT_ROOT}/build}"
RESULT_DIR="${RESULT_DIR:-${PROJECT_ROOT}/benchmark-results/tencent-full-$(date +%Y%m%d-%H%M%S)}"
LOG_FILE="${RESULT_DIR}/full_test.log"

# The first section header is logged before main() performs its checks; create
# the caller-selected result directory up front so tee can open the log.
mkdir -p "${RESULT_DIR}"

# 测试参数
MESSAGES=10000000
RUNS_PER_CONFIG=5
# Formal evidence uses three strictly alternating pairs.  Keep this separate
# from the longer internal channel/strategy matrix above.
FORMAL_RUNS="${FORMAL_RUNS:-3}"
BENCHMARK_CPU="${BENCHMARK_CPU:-0}"
RUN_INTERNAL_TESTS="${RUN_INTERNAL_TESTS:-1}"
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

# ==================== Boost.Asio 协程对标 ====================
run_boost_asio_comparison() {
    log_section "4. Boost.Asio 协程 UDP 公平对标"

    local galay_bin="${BUILD_DIR}/benchmark/cpp/kernel/benchmark_kernel_udp_socket_throughput"
    local asio_bin="${BUILD_DIR}/benchmark/cpp/kernel/benchmark_kernel_compare_boost_asio_coro_udp"
    local result_file="${RESULT_DIR}/boost_asio_coro_comparison.txt"
    local csv_file="${RESULT_DIR}/boost_asio_coro.csv"
    local raw_dir="${RESULT_DIR}/boost_asio_coro_raw"
    local failed_samples=0

    if ! check_binary "${galay_bin}" || ! check_binary "${asio_bin}"; then
        log "Galay/Boost.Asio UDP 对标二进制不存在,跳过对标测试"
        return 1
    fi
    if ! check_command taskset; then
        log "正式对标需要 taskset 将双方固定到同一 CPU"
        return 1
    fi
    if [[ ! "${BENCHMARK_CPU}" =~ ^[0-9]+$ ]]; then
        log "正式对标 CPU 编号无效: ${BENCHMARK_CPU}"
        return 1
    fi
    if [[ ! "${FORMAL_RUNS}" =~ ^[1-9][0-9]*$ ]]; then
        log "正式对标轮数无效: ${FORMAL_RUNS}"
        return 1
    fi

    mkdir -p "${raw_dir}"
    : >"${result_file}"
    printf '%s\n' \
        'implementation,version,backend,coroutine,scenario,clients,workers,payload_bytes,pipeline,warmup_s,duration_s,run,client_sent,client_received,server_received,server_sent,settled_client_sent,settled_client_received,settled_server_received,settled_server_sent,client_pkt_s,server_pkt_s,client_loss_pct,settled_client_loss_pct,runtime_errors,shutdown_errors,status,raw_file' \
        >"${csv_file}"

    value_for() {
        local raw_file="$1"
        local key="$2"
        awk -v wanted="${key}" '
            {
                for (i = 1; i <= NF; ++i) {
                    split($i, pair, "=")
                    if (pair[1] == wanted) {
                        print substr($i, length(wanted) + 2)
                        exit
                    }
                }
            }
        ' "${raw_file}"
    }

    phase_value() {
        local raw_file="$1"
        local phase="$2"
        local key="$3"
        awk -v wanted_phase="${phase}" -v wanted_key="${key}" '
            $1 == wanted_phase {
                for (i = 1; i <= NF; ++i) {
                    split($i, pair, "=")
                    if (pair[1] == wanted_key) {
                        print substr($i, length(wanted_key) + 2)
                        exit
                    }
                }
            }
        ' "${raw_file}"
    }

    run_formal_sample() {
        local implementation="$1"
        local run="$2"
        local raw_file="$3"
        local rc=0
        if [[ "${implementation}" == "galay" ]]; then
            if taskset -c "${BENCHMARK_CPU}" "${galay_bin}" >"${raw_file}" 2>&1; then
                rc=0
            else
                rc=$?
            fi
        else
            if taskset -c "${BENCHMARK_CPU}" "${asio_bin}" \
                --clients 100 --workers 4 --size 256 --warmup 1 --duration 5 \
                >"${raw_file}" 2>&1; then
                rc=0
            else
                rc=$?
            fi
        fi

        local status
        status="$(value_for "${raw_file}" status)"
        local valid=true
        if [[ "${rc}" -ne 0 || "${status}" != "ok" ]]; then
            valid=false
        fi

        local version backend coroutine scenario clients workers payload pipeline warmup duration
        local client_sent client_received server_received server_sent
        local settled_client_sent settled_client_received settled_server_received settled_server_sent
        local client_pkt_s server_pkt_s client_loss_pct settled_client_loss_pct runtime_errors shutdown_errors
        version="$(value_for "${raw_file}" version)"
        backend="$(value_for "${raw_file}" backend)"
        coroutine="$(value_for "${raw_file}" coroutine)"
        scenario="$(value_for "${raw_file}" scenario)"
        clients="$(value_for "${raw_file}" clients)"
        workers="$(value_for "${raw_file}" workers)"
        payload="$(value_for "${raw_file}" payload_bytes)"
        pipeline="$(value_for "${raw_file}" pipeline)"
        warmup="$(value_for "${raw_file}" warmup_s)"
        duration="$(value_for "${raw_file}" duration_s)"
        client_sent="$(phase_value "${raw_file}" measured client_sent)"
        client_received="$(phase_value "${raw_file}" measured client_received)"
        server_received="$(phase_value "${raw_file}" measured server_received)"
        server_sent="$(phase_value "${raw_file}" measured server_sent)"
        settled_client_sent="$(phase_value "${raw_file}" settled client_sent)"
        settled_client_received="$(phase_value "${raw_file}" settled client_received)"
        settled_server_received="$(phase_value "${raw_file}" settled server_received)"
        settled_server_sent="$(phase_value "${raw_file}" settled server_sent)"
        client_pkt_s="$(phase_value "${raw_file}" measured client_pkt_s)"
        server_pkt_s="$(phase_value "${raw_file}" measured server_pkt_s)"
        client_loss_pct="$(phase_value "${raw_file}" measured client_loss_pct)"
        settled_client_loss_pct="$(phase_value "${raw_file}" settled settled_loss_pct)"
        if [[ -z "${settled_client_loss_pct}" ]]; then
            settled_client_loss_pct="$(phase_value "${raw_file}" settled client_loss_pct)"
        fi
        runtime_errors="$(phase_value "${raw_file}" measured runtime_errors)"
        shutdown_errors="$(phase_value "${raw_file}" measured shutdown_errors)"

        printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
            "${implementation}" "${version:-unknown}" "${backend:-unknown}" "${coroutine:-unknown}" \
            "${scenario:-udp-echo}" "${clients:-100}" "${workers:-4}" "${payload:-256}" "${pipeline:-1}" \
            "${warmup:-1}" "${duration:-5}" "${run}" "${client_sent:-0}" "${client_received:-0}" \
            "${server_received:-0}" "${server_sent:-0}" "${settled_client_sent:-0}" \
            "${settled_client_received:-0}" "${settled_server_received:-0}" "${settled_server_sent:-0}" \
            "${client_pkt_s:-0}" "${server_pkt_s:-0}" "${client_loss_pct:-0}" \
            "${settled_client_loss_pct:-0}" "${runtime_errors:-0}" "${shutdown_errors:-0}" \
            "${status:-missing}" "${raw_file#${PROJECT_ROOT}/}" >>"${csv_file}"

        printf '%s,%s,raw_file=%s\n' "${implementation}" "${run}" "${raw_file#${PROJECT_ROOT}/}" >>"${result_file}"
        cat "${raw_file}" >>"${result_file}"
        if [[ "${valid}" == true ]]; then
            return 0
        fi
        return 1
    }

    printf 'benchmark_cpu=%s\n' "${BENCHMARK_CPU}" >>"${result_file}"
    for run in $(seq 1 "${FORMAL_RUNS}"); do
        log "  Galay UDP run ${run}/${FORMAL_RUNS}..."
        if ! run_formal_sample galay "${run}" \
            "${raw_dir}/galay_udp_run${run}.txt"; then
            failed_samples=$((failed_samples + 1))
        fi
        log "  Boost.Asio coroutine UDP run ${run}/${FORMAL_RUNS}..."
        if ! run_formal_sample boost.asio "${run}" \
            "${raw_dir}/boost_asio_coro_udp_run${run}.txt"; then
            failed_samples=$((failed_samples + 1))
        fi
    done

    log "Boost.Asio 协程对标结果已保存到: ${result_file}"
    log "Boost.Asio 协程对标 CSV 已保存到: ${csv_file}"
    if [[ "${failed_samples}" -ne 0 ]]; then
        log "正式对标失败样本数: ${failed_samples}"
        return 1
    fi
}

# ==================== Boost.Asio TCP 协程对标 ====================
run_boost_asio_tcp_comparison() {
    log_section "5. Boost.Asio 协程 TCP 公平对标"

    local galay_bin="${BUILD_DIR}/benchmark/cpp/kernel/benchmark_kernel_tcp_socket_fair_throughput"
    local asio_bin="${BUILD_DIR}/benchmark/cpp/kernel/benchmark_kernel_compare_boost_asio_coro_tcp"
    local result_file="${RESULT_DIR}/boost_asio_coro_tcp_comparison.txt"
    local csv_file="${RESULT_DIR}/boost_asio_coro_tcp.csv"
    local raw_dir="${RESULT_DIR}/boost_asio_coro_tcp_raw"
    local failed_samples=0

    if ! check_binary "${galay_bin}" || ! check_binary "${asio_bin}"; then
        log "Galay/Boost.Asio TCP 对标二进制不存在,跳过对标测试"
        return 1
    fi
    if ! check_command taskset; then
        log "正式 TCP 对标需要 taskset 将双方固定到同一 CPU"
        return 1
    fi
    if [[ ! "${BENCHMARK_CPU}" =~ ^[0-9]+$ ]]; then
        log "正式对标 CPU 编号无效: ${BENCHMARK_CPU}"
        return 1
    fi
    if [[ ! "${FORMAL_RUNS}" =~ ^[1-9][0-9]*$ ]]; then
        log "正式对标轮数无效: ${FORMAL_RUNS}"
        return 1
    fi

    mkdir -p "${raw_dir}"
    : >"${result_file}"
    printf '%s\n' \
        'implementation,version,backend,coroutine,scenario,clients,workers,payload_bytes,pipeline,warmup_s,duration_s,run,client_sent,client_received,server_received,server_sent,settled_client_sent,settled_client_received,settled_server_received,settled_server_sent,client_pkt_s,server_pkt_s,client_loss_pct,settled_client_loss_pct,runtime_errors,shutdown_errors,status,raw_file' \
        >"${csv_file}"

    value_for_tcp() {
        local raw_file="$1"
        local key="$2"
        awk -v wanted="${key}" '
            {
                for (i = 1; i <= NF; ++i) {
                    split($i, pair, "=")
                    if (pair[1] == wanted) {
                        print substr($i, length(wanted) + 2)
                        exit
                    }
                }
            }
        ' "${raw_file}"
    }

    phase_value_tcp() {
        local raw_file="$1"
        local phase="$2"
        local key="$3"
        awk -v wanted_phase="${phase}" -v wanted_key="${key}" '
            $1 == wanted_phase {
                for (i = 1; i <= NF; ++i) {
                    split($i, pair, "=")
                    if (pair[1] == wanted_key) {
                        print substr($i, length(wanted_key) + 2)
                        exit
                    }
                }
            }
        ' "${raw_file}"
    }

    run_tcp_formal_sample() {
        local implementation="$1"
        local run="$2"
        local raw_file="$3"
        local rc=0
        if [[ "${implementation}" == "galay" ]]; then
            if taskset -c "${BENCHMARK_CPU}" "${galay_bin}" >"${raw_file}" 2>&1; then
                rc=0
            else
                rc=$?
            fi
        else
            if taskset -c "${BENCHMARK_CPU}" "${asio_bin}" \
                --clients 100 --workers 4 --size 256 --warmup 1 --duration 5 \
                >"${raw_file}" 2>&1; then
                rc=0
            else
                rc=$?
            fi
        fi

        local status
        status="$(value_for_tcp "${raw_file}" status)"
        local valid=true
        if [[ "${rc}" -ne 0 || "${status}" != "ok" ]]; then
            valid=false
        fi

        local version backend coroutine scenario clients workers payload pipeline warmup duration
        local client_sent client_received server_received server_sent
        local settled_client_sent settled_client_received settled_server_received settled_server_sent
        local client_pkt_s server_pkt_s client_loss_pct settled_client_loss_pct runtime_errors shutdown_errors
        version="$(value_for_tcp "${raw_file}" version)"
        backend="$(value_for_tcp "${raw_file}" backend)"
        coroutine="$(value_for_tcp "${raw_file}" coroutine)"
        scenario="$(value_for_tcp "${raw_file}" scenario)"
        clients="$(value_for_tcp "${raw_file}" clients)"
        workers="$(value_for_tcp "${raw_file}" workers)"
        payload="$(value_for_tcp "${raw_file}" payload_bytes)"
        pipeline="$(value_for_tcp "${raw_file}" pipeline)"
        warmup="$(value_for_tcp "${raw_file}" warmup_s)"
        duration="$(value_for_tcp "${raw_file}" duration_s)"
        client_sent="$(phase_value_tcp "${raw_file}" measured client_sent)"
        client_received="$(phase_value_tcp "${raw_file}" measured client_received)"
        server_received="$(phase_value_tcp "${raw_file}" measured server_received)"
        server_sent="$(phase_value_tcp "${raw_file}" measured server_sent)"
        settled_client_sent="$(phase_value_tcp "${raw_file}" settled client_sent)"
        settled_client_received="$(phase_value_tcp "${raw_file}" settled client_received)"
        settled_server_received="$(phase_value_tcp "${raw_file}" settled server_received)"
        settled_server_sent="$(phase_value_tcp "${raw_file}" settled server_sent)"
        client_pkt_s="$(phase_value_tcp "${raw_file}" measured client_pkt_s)"
        server_pkt_s="$(phase_value_tcp "${raw_file}" measured server_pkt_s)"
        client_loss_pct="$(phase_value_tcp "${raw_file}" measured client_loss_pct)"
        settled_client_loss_pct="$(phase_value_tcp "${raw_file}" settled settled_loss_pct)"
        if [[ -z "${settled_client_loss_pct}" ]]; then
            settled_client_loss_pct="$(phase_value_tcp "${raw_file}" settled client_loss_pct)"
        fi
        runtime_errors="$(phase_value_tcp "${raw_file}" measured runtime_errors)"
        shutdown_errors="$(phase_value_tcp "${raw_file}" measured shutdown_errors)"

        printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
            "${implementation}" "${version:-unknown}" "${backend:-unknown}" "${coroutine:-unknown}" \
            "${scenario:-tcp-echo}" "${clients:-100}" "${workers:-4}" "${payload:-256}" "${pipeline:-1}" \
            "${warmup:-1}" "${duration:-5}" "${run}" "${client_sent:-0}" "${client_received:-0}" \
            "${server_received:-0}" "${server_sent:-0}" "${settled_client_sent:-0}" \
            "${settled_client_received:-0}" "${settled_server_received:-0}" "${settled_server_sent:-0}" \
            "${client_pkt_s:-0}" "${server_pkt_s:-0}" "${client_loss_pct:-0}" \
            "${settled_client_loss_pct:-0}" "${runtime_errors:-0}" "${shutdown_errors:-0}" \
            "${status:-missing}" "${raw_file#${PROJECT_ROOT}/}" >>"${csv_file}"

        printf '%s,%s,raw_file=%s\n' "${implementation}" "${run}" "${raw_file#${PROJECT_ROOT}/}" >>"${result_file}"
        cat "${raw_file}" >>"${result_file}"
        if [[ "${valid}" == true ]]; then
            return 0
        fi
        return 1
    }

    printf 'benchmark_cpu=%s\n' "${BENCHMARK_CPU}" >>"${result_file}"
    for run in $(seq 1 "${FORMAL_RUNS}"); do
        log "  Galay TCP run ${run}/${FORMAL_RUNS}..."
        if ! run_tcp_formal_sample galay "${run}" \
            "${raw_dir}/galay_tcp_run${run}.txt"; then
            failed_samples=$((failed_samples + 1))
        fi
        log "  Boost.Asio coroutine TCP run ${run}/${FORMAL_RUNS}..."
        if ! run_tcp_formal_sample boost.asio "${run}" \
            "${raw_dir}/boost_asio_coro_tcp_run${run}.txt"; then
            failed_samples=$((failed_samples + 1))
        fi
    done

    log "Boost.Asio 协程 TCP 对标结果已保存到: ${result_file}"
    log "Boost.Asio 协程 TCP 对标 CSV 已保存到: ${csv_file}"
    if [[ "${failed_samples}" -ne 0 ]]; then
        log "正式 TCP 对标失败样本数: ${failed_samples}"
        return 1
    fi
}

# ==================== 结果汇总 ====================
generate_summary() {
    log_section "6. 生成测试报告"

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

        # Boost.Asio 协程对标统计
        if [ -f "${RESULT_DIR}/boost_asio_coro_comparison.txt" ]; then
            local asio_runs=$(grep -c '^boost.asio,' "${RESULT_DIR}/boost_asio_coro_comparison.txt" || echo 0)
            local galay_runs=$(grep -c '^galay,' "${RESULT_DIR}/boost_asio_coro_comparison.txt" || echo 0)
            echo "Boost.Asio 协程 UDP 对标: Galay=${galay_runs}, Boost.Asio=${asio_runs}"
        fi
        if [ -f "${RESULT_DIR}/boost_asio_coro_tcp_comparison.txt" ]; then
            local asio_tcp_runs=$(grep -c '^boost.asio,' "${RESULT_DIR}/boost_asio_coro_tcp_comparison.txt" || echo 0)
            local galay_tcp_runs=$(grep -c '^galay,' "${RESULT_DIR}/boost_asio_coro_tcp_comparison.txt" || echo 0)
            echo "Boost.Asio 协程 TCP 对标: Galay=${galay_tcp_runs}, Boost.Asio=${asio_tcp_runs}"
        fi

        echo ""
        echo "--- 下一步建议 ---"
        echo "1. 运行 NUMA 感知测试: ${SCRIPT_DIR}/tencent_numa_test.sh"
        echo "2. 运行 perf 性能分析: ${SCRIPT_DIR}/tencent_perf_analysis.sh"
        echo "3. 使用 Python 脚本分析结果:"
        echo "   解析 benchmark/cpp/kernel/compare/boost-asio-coro 的 measured 行并按中位数汇总"
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
    local formal_status=0
    collect_system_info || log "系统信息收集失败"
    if [[ "${RUN_INTERNAL_TESTS}" == "1" ]]; then
        run_standard_tests || log "标准测试失败"
        run_strategy_comparison || log "策略对比失败"
    else
        log "RUN_INTERNAL_TESTS=0: 跳过内部 channel/策略矩阵"
    fi
    if ! run_boost_asio_comparison; then
        log "Boost.Asio 协程 UDP 对标失败"
        formal_status=1
    fi
    if ! run_boost_asio_tcp_comparison; then
        log "Boost.Asio 协程 TCP 对标失败"
        formal_status=1
    fi
    generate_summary

    log_section "测试完成"
    log "结束时间: $(date)"
    log "所有结果保存在: ${RESULT_DIR}"

    echo ""
    echo "完整日志: ${LOG_FILE}"
    return "${formal_status}"
}

# 运行主流程
main "$@"
