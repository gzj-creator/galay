#!/bin/bash
# Tencent 机器 NUMA 感知测试脚本
# 测试跨 NUMA 节点 vs 同节点性能差异,CPU 亲和性影响

set -euo pipefail

# ==================== 配置 ====================
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${PROJECT_ROOT}/build"
RESULT_DIR="${PROJECT_ROOT}/benchmark-results/tencent-numa-$(date +%Y%m%d-%H%M%S)"
LOG_FILE="${RESULT_DIR}/numa_test.log"

# 测试参数
MESSAGES=5000000
RUNS_PER_CONFIG=5
CAPACITY=4096

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

check_numa_available() {
    if ! command -v numactl &> /dev/null; then
        log "错误: numactl 命令不可用"
        log "请安装: yum install numactl 或 apt-get install numactl"
        return 1
    fi

    local numa_nodes=$(numactl --hardware | grep "available:" | awk '{print $2}')
    if [ "${numa_nodes}" -lt 2 ]; then
        log "警告: 系统只有 ${numa_nodes} 个 NUMA 节点"
        log "NUMA 测试需要至少 2 个节点"
        return 1
    fi

    log "检测到 ${numa_nodes} 个 NUMA 节点"
    return 0
}

detect_numa_topology() {
    log_section "检测 NUMA 拓扑"

    local topo_file="${RESULT_DIR}/numa_topology.txt"

    {
        echo "=== NUMA 硬件信息 ==="
        numactl --hardware
        echo ""

        echo "=== 节点 CPU 分布 ==="
        for node in /sys/devices/system/node/node*; do
            if [ -d "$node" ]; then
                local node_id=$(basename "$node" | sed 's/node//')
                echo "Node ${node_id}: $(cat ${node}/cpulist)"
            fi
        done
        echo ""

        echo "=== 节点内存信息 ==="
        numactl --show
        echo ""

        echo "=== NUMA 距离矩阵 ==="
        if [ -f /sys/devices/system/node/node0/distance ]; then
            for node in /sys/devices/system/node/node*; do
                if [ -d "$node" ]; then
                    local node_id=$(basename "$node" | sed 's/node//')
                    echo "Node ${node_id}: $(cat ${node}/distance)"
                fi
            done
        fi
        echo ""

    } > "${topo_file}"

    cat "${topo_file}" | tee -a "${LOG_FILE}"
}

get_node_cpus() {
    local node=$1
    local cpulist_file="/sys/devices/system/node/node${node}/cpulist"

    if [ -f "${cpulist_file}" ]; then
        cat "${cpulist_file}"
    else
        # 回退到 numactl
        numactl --hardware | grep "node ${node} cpus:" | sed 's/.*cpus: //'
    fi
}

expand_cpu_range() {
    local range=$1
    python3 -c "
import sys
result = []
for part in '${range}'.split(','):
    if '-' in part:
        start, end = map(int, part.split('-'))
        result.extend(range(start, end + 1))
    else:
        result.append(int(part))
print(','.join(map(str, result[:2])))  # 只取前2个CPU
"
}

# ==================== 同节点测试 ====================
run_same_node_tests() {
    log_section "同 NUMA 节点测试"

    local mpsc_bin="${BUILD_DIR}/benchmark/cpp/kernel/benchmark_kernel_mpsc_channel_throughput"
    if [ ! -x "${mpsc_bin}" ]; then
        log "错误: 二进制文件不存在: ${mpsc_bin}"
        return 1
    fi

    local result_file="${RESULT_DIR}/same_node.jsonl"

    # 获取 NUMA 节点数量
    local num_nodes=$(numactl --hardware | grep "available:" | awk '{print $2}')

    # 在每个节点上测试
    for node in $(seq 0 $((num_nodes - 1))); do
        local node_cpus=$(get_node_cpus ${node})
        log "测试 Node ${node} (CPUs: ${node_cpus})"

        # 测试不同生产者数量
        for producers in 1 2 4 8; do
            log "  ${producers} 生产者..."

            for run in $(seq 1 ${RUNS_PER_CONFIG}); do
                # 使用 numactl 绑定到特定节点
                numactl --cpunodebind=${node} --membind=${node} \
                    "${mpsc_bin}" \
                    --producers "${producers}" \
                    --messages "${MESSAGES}" \
                    --capacity "${CAPACITY}" \
                    --json >> "${result_file}" 2>> "${LOG_FILE}" || {
                    log "    运行 ${run} 失败"
                }
            done
        done
    done

    log "同节点测试结果已保存到: ${result_file}"
}

# ==================== 跨节点测试 ====================
run_cross_node_tests() {
    log_section "跨 NUMA 节点测试"

    local mpsc_bin="${BUILD_DIR}/benchmark/cpp/kernel/benchmark_kernel_mpsc_channel_throughput"
    if [ ! -x "${mpsc_bin}" ]; then
        log "错误: 二进制文件不存在: ${mpsc_bin}"
        return 1
    fi

    local result_file="${RESULT_DIR}/cross_node.jsonl"

    # 获取两个不同节点的 CPU
    local node0_cpus=$(get_node_cpus 0)
    local node1_cpus=$(get_node_cpus 1)

    if [ -z "${node0_cpus}" ] || [ -z "${node1_cpus}" ]; then
        log "错误: 无法获取节点 CPU 信息"
        return 1
    fi

    log "Node 0 CPUs: ${node0_cpus}"
    log "Node 1 CPUs: ${node1_cpus}"

    # 测试跨节点配置
    for producers in 2 4 8; do
        log "测试 ${producers} 生产者跨节点..."

        for run in $(seq 1 ${RUNS_PER_CONFIG}); do
            # 生产者在 node0,消费者在 node1
            # 注意: 这需要程序支持 CPU 亲和性设置
            numactl --cpunodebind=0,1 --membind=0 \
                "${mpsc_bin}" \
                --producers "${producers}" \
                --messages "${MESSAGES}" \
                --capacity "${CAPACITY}" \
                --json >> "${result_file}" 2>> "${LOG_FILE}" || {
                log "  运行 ${run} 失败"
            }
        done
    done

    log "跨节点测试结果已保存到: ${result_file}"
}

# ==================== CPU 亲和性测试 ====================
run_affinity_tests() {
    log_section "CPU 亲和性测试"

    local mpsc_bin="${BUILD_DIR}/benchmark/cpp/kernel/benchmark_kernel_mpsc_channel_throughput"
    if [ ! -x "${mpsc_bin}" ]; then
        log "错误: 二进制文件不存在: ${mpsc_bin}"
        return 1
    fi

    local result_file="${RESULT_DIR}/affinity.jsonl"

    # 获取第一个节点的前几个 CPU
    local node0_cpus=$(get_node_cpus 0)
    local cpu_list=$(expand_cpu_range "${node0_cpus}")

    log "使用 CPU 列表: ${cpu_list}"

    # 测试配置
    local configs=(
        "no_affinity::"  # 无亲和性
        "node_bind:0:--cpunodebind=0"  # 绑定节点
        "cpu_bind:${cpu_list}:--physcpubind=${cpu_list}"  # 绑定特定 CPU
    )

    for config in "${configs[@]}"; do
        IFS=':' read -r name cpus args <<< "${config}"
        log "测试亲和性配置: ${name}"

        for producers in 2 4 8; do
            log "  ${producers} 生产者..."

            for run in $(seq 1 ${RUNS_PER_CONFIG}); do
                if [ -z "${args}" ]; then
                    # 无亲和性
                    "${mpsc_bin}" \
                        --producers "${producers}" \
                        --messages "${MESSAGES}" \
                        --capacity "${CAPACITY}" \
                        --json >> "${result_file}" 2>> "${LOG_FILE}" || true
                else
                    # 有亲和性
                    numactl ${args} \
                        "${mpsc_bin}" \
                        --producers "${producers}" \
                        --messages "${MESSAGES}" \
                        --capacity "${CAPACITY}" \
                        --json >> "${result_file}" 2>> "${LOG_FILE}" || true
                fi
            done
        done
    done

    log "亲和性测试结果已保存到: ${result_file}"
}

# ==================== 内存分配策略测试 ====================
run_memory_policy_tests() {
    log_section "NUMA 内存分配策略测试"

    local mpsc_bin="${BUILD_DIR}/benchmark/cpp/kernel/benchmark_kernel_mpsc_channel_throughput"
    if [ ! -x "${mpsc_bin}" ]; then
        log "错误: 二进制文件不存在: ${mpsc_bin}"
        return 1
    fi

    local result_file="${RESULT_DIR}/memory_policy.jsonl"

    # 测试不同内存分配策略
    local policies=(
        "default:"
        "interleave:--interleave=all"
        "preferred:--preferred=0"
        "local:--localalloc"
    )

    for policy in "${policies[@]}"; do
        IFS=':' read -r name args <<< "${policy}"
        log "测试内存策略: ${name}"

        for producers in 4 8; do
            log "  ${producers} 生产者..."

            for run in $(seq 1 ${RUNS_PER_CONFIG}); do
                if [ -z "${args}" ]; then
                    "${mpsc_bin}" \
                        --producers "${producers}" \
                        --messages "${MESSAGES}" \
                        --capacity "${CAPACITY}" \
                        --json >> "${result_file}" 2>> "${LOG_FILE}" || true
                else
                    numactl ${args} \
                        "${mpsc_bin}" \
                        --producers "${producers}" \
                        --messages "${MESSAGES}" \
                        --capacity "${CAPACITY}" \
                        --json >> "${result_file}" 2>> "${LOG_FILE}" || true
                fi
            done
        done
    done

    log "内存策略测试结果已保存到: ${result_file}"
}

# ==================== 结果分析 ====================
analyze_results() {
    log_section "NUMA 测试结果分析"

    local summary_file="${RESULT_DIR}/numa_summary.txt"

    {
        echo "=========================================="
        echo "NUMA 性能测试报告"
        echo "=========================================="
        echo ""
        echo "测试时间: $(date)"
        echo "结果目录: ${RESULT_DIR}"
        echo ""

        echo "--- 测试统计 ---"

        if [ -f "${RESULT_DIR}/same_node.jsonl" ]; then
            local same_node_runs=$(wc -l < "${RESULT_DIR}/same_node.jsonl")
            echo "同节点测试: ${same_node_runs} 次运行"
        fi

        if [ -f "${RESULT_DIR}/cross_node.jsonl" ]; then
            local cross_node_runs=$(wc -l < "${RESULT_DIR}/cross_node.jsonl")
            echo "跨节点测试: ${cross_node_runs} 次运行"
        fi

        if [ -f "${RESULT_DIR}/affinity.jsonl" ]; then
            local affinity_runs=$(wc -l < "${RESULT_DIR}/affinity.jsonl")
            echo "亲和性测试: ${affinity_runs} 次运行"
        fi

        if [ -f "${RESULT_DIR}/memory_policy.jsonl" ]; then
            local memory_runs=$(wc -l < "${RESULT_DIR}/memory_policy.jsonl")
            echo "内存策略测试: ${memory_runs} 次运行"
        fi

        echo ""
        echo "--- 性能对比分析 ---"

        # 使用 Python 快速分析
        if command -v python3 &> /dev/null; then
            python3 - <<'EOF'
import json
import sys
from pathlib import Path
from statistics import mean

def analyze_file(filepath):
    if not Path(filepath).exists():
        return None

    results = []
    with open(filepath) as f:
        for line in f:
            try:
                data = json.loads(line)
                if data.get('valid', False):
                    results.append(data['messages_per_second'] / 1e6)
            except:
                pass

    if results:
        return {
            'count': len(results),
            'mean': mean(results),
            'min': min(results),
            'max': max(results)
        }
    return None

result_dir = Path('${RESULT_DIR}')

files = {
    '同节点': result_dir / 'same_node.jsonl',
    '跨节点': result_dir / 'cross_node.jsonl',
    '亲和性': result_dir / 'affinity.jsonl',
    '内存策略': result_dir / 'memory_policy.jsonl',
}

for name, filepath in files.items():
    stats = analyze_file(filepath)
    if stats:
        print(f"{name:12s}: {stats['mean']:8.2f} M/s (min: {stats['min']:7.2f}, max: {stats['max']:7.2f}, n={stats['count']})")
EOF
        else
            echo "Python3 不可用,跳过自动分析"
        fi

        echo ""
        echo "--- 关键发现 ---"
        echo "1. 检查同节点 vs 跨节点的性能差异"
        echo "2. 评估 CPU 亲和性的影响"
        echo "3. 比较不同内存分配策略"
        echo "4. 识别 NUMA 相关的性能瓶颈"
        echo ""

        echo "--- 生成的文件 ---"
        ls -lh "${RESULT_DIR}" | tail -n +2
        echo ""

        echo "--- 下一步建议 ---"
        echo "1. 运行详细的 perf 分析: ${SCRIPT_DIR}/tencent_perf_analysis.sh"
        echo "2. 在最佳 NUMA 配置下重新运行完整测试套件"
        echo "3. 使用 numastat 监控运行时 NUMA 统计"
        echo ""

    } > "${summary_file}"

    cat "${summary_file}"
}

# ==================== 主流程 ====================
main() {
    log_section "NUMA 感知性能测试"
    log "开始时间: $(date)"

    # 创建结果目录
    mkdir -p "${RESULT_DIR}"
    log "结果目录: ${RESULT_DIR}"

    # 检查 NUMA 可用性
    if ! check_numa_available; then
        log "NUMA 不可用,退出测试"
        exit 1
    fi

    # 检测拓扑
    detect_numa_topology

    # 执行测试
    run_same_node_tests || log "同节点测试失败"
    run_cross_node_tests || log "跨节点测试失败"
    run_affinity_tests || log "亲和性测试失败"
    run_memory_policy_tests || log "内存策略测试失败"

    # 分析结果
    analyze_results

    log_section "测试完成"
    log "结束时间: $(date)"
    log "所有结果保存在: ${RESULT_DIR}"

    echo ""
    echo "完整日志: ${LOG_FILE}"
}

# 运行主流程
main "$@"
