/**
 * @file b40_operation_completion_pressure.cc
 * @brief 批量 typed completion 压力：结果/恢复权一次移交，延迟 physical drain。
 *
 * 使用已有非拥有 C resume hook 计数，不运行网络或 scheduler，也不声称测量
 * accept 吞吐。固定栈上存储；batch=64/1024、每场景 1M 次、内置一次预热。
 */
#include <galay/cpp/galay-kernel/core/operation_completion.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <optional>

using namespace galay::kernel;

namespace {
constexpr std::uint64_t kOperations = 1'048'576;

struct Probe {
    detail::ResumeTokenHeader header;
    std::uint64_t wakes = 0;
};

const detail::ResumeTokenHooks kHooks{
    .owner_scheduler = [](void*) noexcept -> Scheduler* { return nullptr; },
    .request_resume = [](void* state) noexcept {
        ++static_cast<Probe*>(state)->wakes;
        return true;
    },
};

struct Measurement {
    std::uint64_t errors = 0;
    std::uint64_t wakes = 0;
    std::uint64_t checksum = 0;
    double ns_per_op = 0;
};

template <std::size_t Batch>
Measurement measure()
{
    std::array<std::optional<OperationCompletion<std::uint64_t>>, Batch> operations;
    Probe probe{.header = {&kHooks}};
    Measurement result;
    const auto start = std::chrono::steady_clock::now();
    for (std::uint64_t base = 0; base < kOperations; base += Batch) {
        for (std::size_t i = 0; i < Batch; ++i) {
            auto& op = operations[i].emplace(
                OperationKey{static_cast<uint32_t>(i), static_cast<uint32_t>(base + 1)},
                Waker(detail::ResumeToken::fromNonOwningCCoroutine(&probe)));
            result.errors += !op.markSubmitted();
            result.errors += !op.addPhysicalReference();
        }
        asm volatile("" : : "g"(operations.data()) : "memory");
        for (std::size_t i = 0; i < Batch; ++i) {
            auto& op = *operations[i];
            result.errors += !op.tryComplete(CompletionReason::kReady, base + i + 1);
            result.errors += op.tryComplete(CompletionReason::kTimedOut, 0);
        }
        asm volatile("" : : "g"(operations.data()) : "memory");
        for (auto& op : operations) {
            auto released = op->releasePhysicalReference();
            result.errors += !released || !*released;
            auto resume = op->takeResume();
            if (resume) {
                std::move(*resume).resume();
            } else {
                ++result.errors;
            }
            auto value = op->takeResult();
            if (value) {
                result.checksum += *value;
            } else {
                ++result.errors;
            }
            op.reset();
        }
    }
    const auto ns = std::chrono::duration<double, std::nano>(
        std::chrono::steady_clock::now() - start).count();
    result.ns_per_op = ns / kOperations;
    result.wakes = probe.wakes;
    return result;
}

template <std::size_t Batch>
bool run()
{
    const auto warmup = measure<Batch>();
    const auto sample = measure<Batch>();
    const auto valid = [](const Measurement& m) {
        return m.errors == 0 && m.wakes == kOperations &&
            m.checksum == kOperations * (kOperations + 1) / 2;
    };
    std::cout << "OperationCompletion batch=" << Batch << " operations=" << kOperations
              << " wakes=" << sample.wakes << " checksum=" << sample.checksum
              << " errors=" << sample.errors << " ns_per_op=" << std::fixed
              << std::setprecision(3) << sample.ns_per_op << '\n';
    return valid(warmup) && valid(sample);
}
} // namespace

int main()
{
    std::cout << "sizeof_completion=" << sizeof(OperationCompletion<std::uint64_t>)
              << " warmup=1\n";
    if (!run<64>() || !run<1024>()) {
        std::cerr << "B40-OperationCompletionPressure FAIL\n";
        return 1;
    }
    std::cout << "B40-OperationCompletionPressure PASS\n";
}
