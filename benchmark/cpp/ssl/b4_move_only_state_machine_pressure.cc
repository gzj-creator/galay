/**
 * @file b4_move_only_state_machine_pressure.cc
 * @brief SSL move-only 状态机与 operation driver 构造/move 压力基准。
 *
 * @details 不连接真实 TLS 服务，仅覆盖本地状态对象的构造与 move 路径：
 * - SslOperationDriver: 构造、move 构造、move 赋值、vector 迁移
 * - SslLinearMachine: 节点队列构造、move 构造、move 赋值、vector 迁移
 * - SslStateMachineAwaitable: 地址稳定类型，仅做构造压力，不做 move
 */

#include <galay/cpp/galay-ssl/async/awaitable.h>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <expected>
#include <iostream>
#include <string_view>
#include <vector>

using namespace galay::ssl;

namespace {

using BenchResult = std::expected<size_t, SslError>;

struct BenchFlow {
    size_t local_steps = 0;

    void onLocal(SslBuilderOps<BenchResult, 8>&)
    {
        ++local_steps;
    }

    void onFinish(SslBuilderOps<BenchResult, 8>& ops)
    {
        ops.complete(BenchResult{local_steps});
    }
};

using LinearMachineT = galay::ssl::detail::SslLinearMachine<BenchResult, 8, BenchFlow>;

struct BenchMetrics {
    uint64_t driver_moves = 0;
    uint64_t machine_moves = 0;
    uint64_t awaitable_constructs = 0;
    uint64_t retained_objects = 0;
};

bool parseSize(const char* text, size_t min_value, size_t* value)
{
    size_t parsed = 0;
    const char* end = text + std::char_traits<char>::length(text);
    const auto result = std::from_chars(text, end, parsed);
    if (result.ec != std::errc() || result.ptr != end || parsed < min_value) {
        return false;
    }
    *value = parsed;
    return true;
}

void printUsage(const char* program)
{
    std::cerr << "Usage: " << program << " [iterations>=1] [local_nodes>=1]\n";
}

LinearMachineT::NodeList makeNodes(size_t local_nodes)
{
    LinearMachineT::NodeList nodes;
    nodes.reserve(local_nodes + 1);
    for (size_t i = 0; i < local_nodes; ++i) {
        nodes.push_back(LinearMachineT::template makeLocalNode<&BenchFlow::onLocal>());
    }
    nodes.push_back(LinearMachineT::template makeFinishNode<&BenchFlow::onFinish>());
    return nodes;
}

void runDriverPressure(size_t iterations, BenchMetrics* metrics)
{
    std::vector<SslOperationDriver> retained;
    retained.reserve(64);

    SslOperationDriver assigned(nullptr);
    for (size_t i = 0; i < iterations; ++i) {
        SslOperationDriver driver(nullptr);
        SslOperationDriver moved(std::move(driver));
        assigned = std::move(moved);

        retained.emplace_back(nullptr);
        retained.push_back(std::move(assigned));
        if (retained.size() >= 128) {
            metrics->retained_objects += retained.size();
            retained.clear();
        }
        metrics->driver_moves += 3;
    }
    metrics->retained_objects += retained.size();
}

void runLinearMachinePressure(size_t iterations, size_t local_nodes, BenchMetrics* metrics)
{
    std::vector<LinearMachineT> retained;
    retained.reserve(32);

    BenchFlow flow;
    LinearMachineT assigned(nullptr, &flow, makeNodes(local_nodes));
    for (size_t i = 0; i < iterations; ++i) {
        LinearMachineT machine(nullptr, &flow, makeNodes(local_nodes));
        LinearMachineT moved(std::move(machine));
        assigned = std::move(moved);

        retained.push_back(LinearMachineT(nullptr, &flow, makeNodes(local_nodes)));
        retained.push_back(std::move(assigned));
        if (retained.size() >= 64) {
            metrics->retained_objects += retained.size();
            retained.clear();
        }
        metrics->machine_moves += 3;
    }
    metrics->retained_objects += retained.size();
}

void runAwaitableConstructPressure(size_t iterations, size_t local_nodes, BenchMetrics* metrics)
{
    BenchFlow flow;
    for (size_t i = 0; i < iterations; ++i) {
        SslStateMachineAwaitable<LinearMachineT> awaitable(
            nullptr,
            nullptr,
            LinearMachineT(nullptr, &flow, makeNodes(local_nodes)));
        if (awaitable.empty()) {
            metrics->awaitable_constructs += 1;
        }
    }
}

} // namespace

int main(int argc, char* argv[])
{
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        printUsage(argv[0]);
        return 0;
    }

    size_t iterations = 100000;
    size_t local_nodes = 4;
    if (argc >= 2 && !parseSize(argv[1], 1, &iterations)) {
        printUsage(argv[0]);
        return 1;
    }
    if (argc >= 3 && !parseSize(argv[2], 1, &local_nodes)) {
        printUsage(argv[0]);
        return 1;
    }

    BenchMetrics metrics;
    const auto start = std::chrono::steady_clock::now();

    runDriverPressure(iterations, &metrics);
    runLinearMachinePressure(iterations, local_nodes, &metrics);
    runAwaitableConstructPressure(iterations, local_nodes, &metrics);

    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto elapsed_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
    const uint64_t total_ops = metrics.driver_moves + metrics.machine_moves + metrics.awaitable_constructs;
    const auto ns_per_op = total_ops == 0 ? 0 : elapsed_ns / static_cast<int64_t>(total_ops);

    std::cout << "ssl_move_only_state_machine_pressure"
              << " iterations=" << iterations
              << " local_nodes=" << local_nodes
              << " driver_moves=" << metrics.driver_moves
              << " machine_moves=" << metrics.machine_moves
              << " awaitable_constructs=" << metrics.awaitable_constructs
              << " retained_objects=" << metrics.retained_objects
              << " elapsed_ns=" << elapsed_ns
              << " ns_per_op=" << ns_per_op
              << std::endl;

    return 0;
}
