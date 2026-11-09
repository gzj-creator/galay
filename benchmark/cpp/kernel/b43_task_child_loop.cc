/**
 * @file b43_task_child_loop.cc
 * @brief 在一个已提交的 Task 根内重复等待子链，隔离每轮 block_on 的固定开销。
 */

#include "benchmark/cpp/common/benchmark_environment.h"
#include <galay/cpp/galay-kernel/core/runtime.h>

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iomanip>
#include <iostream>
#include <string_view>

namespace {

using galay::kernel::Runtime;
using galay::kernel::RuntimeBuilder;
using galay::kernel::Task;
using TaskResult = std::expected<std::uint64_t, galay::kernel::detail::TaskResultError>;

/** @brief 逐层创建、等待并消费现有 Task，完整向根传播执行错误。 */
[[gnu::noinline]] Task<TaskResult> task_chain(unsigned depth)
{
    if (depth == 0) {
        co_return TaskResult(1);
    }
    auto child = co_await task_chain(depth - 1);
    if (!child) {
        co_return std::unexpected(child.error());
    }
    if (!*child) {
        co_return std::unexpected(child->error());
    }
    co_return TaskResult(**child + 1);
}

/** @brief 仅提交一次根任务，把重复子调用留在同一调度器线程中执行。 */
[[gnu::noinline]] Task<TaskResult> repeat_child_chain(unsigned depth, std::size_t iterations)
{
    std::uint64_t checksum = 0;
    for (std::size_t index = 0; index < iterations; ++index) {
        auto child = co_await task_chain(depth);
        if (!child) {
            co_return std::unexpected(child.error());
        }
        if (!*child) {
            co_return std::unexpected(child->error());
        }
        checksum += **child;
    }
    co_return TaskResult(checksum);
}

/** @brief 严格解析十进制输入，不接纳负数、溢出或尾随字符。 */
template <typename Value>
bool parse_number(std::string_view text, Value& value)
{
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}

/** @brief 驱动根任务并检查 Runtime 和子链两层显式结果。 */
bool run_batch(Runtime& runtime, unsigned depth, std::size_t iterations)
{
    auto result = runtime.block_on_cpu(repeat_child_chain(depth, iterations));
    if (!result) {
        std::cerr << "status=error reason=runtime message=" << result.error().message() << '\n';
        return false;
    }
    if (!*result) {
        std::cerr << "status=error reason=task message=" << result->error().message() << '\n';
        return false;
    }
    return **result == static_cast<std::uint64_t>(iterations) * (depth + 1);
}

} // namespace

/** @brief 以 iteration、depth 指定任务数量，输出每个子 Task 节点的完整成本。 */
int main(int argc, char** argv)
{
    std::size_t iterations = 1'000'000;
    unsigned depth = 0;
    if (argc != 3 || !parse_number(std::string_view(argv[1]), iterations) ||
        !parse_number(std::string_view(argv[2]), depth) || iterations == 0 ||
        iterations > 1'000'000'000 || (depth != 0 && depth != 4 && depth != 16)) {
        std::cerr << "usage: benchmark_kernel_task_child_loop <iterations 1..1000000000> <depth 0|4|16>\n";
        return 2;
    }
    if (!galay::benchmark::initialize_benchmark_environment()) {
        return 1;
    }
    Runtime runtime = RuntimeBuilder().io_scheduler_count(1)
        .parallel_scheduler_count(1).build();
    if (!run_batch(runtime, depth, 2'000)) {
        return 1;
    }
    const auto begin = std::chrono::steady_clock::now();
    if (!run_batch(runtime, depth, iterations)) {
        return 1;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - begin).count();
    const auto nodes = static_cast<std::uint64_t>(iterations) * (depth + 1);
    std::cout << "status=ok scenario=task_child_loop depth=" << depth
              << " iterations=" << iterations << " task_nodes=" << nodes
              << " elapsed_ns=" << elapsed << " checksum=" << nodes
              << " ns_per_task_node=" << std::fixed << std::setprecision(3)
              << static_cast<double>(elapsed) / static_cast<double>(nodes) << '\n';
    return std::cout ? 0 : 1;
}
