/**
 * @file b39_operation_state_gate.cc
 * @brief Owner 完成状态机的批量生命周期基准，不是 accept 吞吐基准。
 *
 * 每轮含 construct/submit/retain/winner/loser/drain/destruct；batch=1/64/1024。
 * 各阶段的 compiler memory barrier 防止跨阶段常量折叠；不使用 volatile bool
 * 作为不等价的对照。每种 batch 内置一次预热，正式重复由外部 runner 控制。
 */
#include "../common/benchmark_environment.h"

#include <galay/cpp/galay-kernel/core/operation_state.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>

using namespace galay::kernel;

namespace {

constexpr std::size_t kOperations = 1'048'576;

struct Measurement {
    std::uint64_t winners = 0;
    std::uint64_t drains = 0;
    std::uint64_t errors = 0;
    double ns_per_op = 0;
};

template <std::size_t Batch>
Measurement measure()
{
    // 无动态分配；optional 在固定地址重建，不移动已发布状态。
    std::array<std::optional<OperationState>, Batch> operations;
    Measurement result;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t round = 0; round < kOperations / Batch; ++round) {
        for (std::size_t i = 0; i < Batch; ++i) {
            auto& op = operations[i].emplace(OperationKey{
                static_cast<uint32_t>(i), static_cast<uint32_t>(round + 1)});
            result.errors += !op.mark_submitted();
            result.errors += !op.add_physical_reference();
        }
        // GCC/Clang：假设 adapter 可读写这批 operation，但不生成硬件 fence。
        asm volatile("" : : "g"(operations.data()) : "memory");
        for (std::size_t i = 0; i < Batch; ++i) {
            result.winners += operations[i]->try_complete(
                (round + i) % 2 ? CompletionReason::kReady : CompletionReason::kCancelled);
        }
        asm volatile("" : : "g"(operations.data()) : "memory");
        for (auto& op : operations) {
            result.errors += op->try_complete(CompletionReason::kRuntimeStopped);
            const auto released = op->release_physical_reference();
            result.errors += !released;
            result.drains += op->phase() == OperationPhase::kSafeToResume;
            op.reset();
        }
    }
    const auto ns = std::chrono::duration<double, std::nano>(
        std::chrono::steady_clock::now() - start).count();
    result.ns_per_op = ns / kOperations;
    return result;
}

template <std::size_t Batch>
bool run()
{
    const auto warmup = measure<Batch>();
    const auto sample = measure<Batch>();
    const auto valid = [](const Measurement& m) {
        return m.errors == 0 && m.winners == kOperations && m.drains == kOperations;
    };
    std::cout << "OperationLifecycle batch=" << Batch << " operations=" << kOperations
              << " winners=" << sample.winners << " drains=" << sample.drains
              << " errors=" << sample.errors << " ns_per_op=" << std::fixed
              << std::setprecision(3) << sample.ns_per_op << '\n';
    return valid(warmup) && valid(sample);
}

} // namespace

int main()
{
    if (!galay::benchmark::initialize_benchmark_environment()) {
        return 1;
    }

    std::cout << "sizeof_state=" << sizeof(OperationState)
              << " alignof_state=" << alignof(OperationState) << " warmup=1\n";
    if (!run<1>() || !run<64>() || !run<1024>()) {
        std::cerr << "B39-OperationStateGate FAIL\n";
        return 1;
    }
    std::cout << "B39-OperationStateGate PASS\n";
}
