/**
 * @file t208_task_result_state.cc
 * @brief 验证任务结果错误交付和并发单次消费协议。
 */

#include <galay/cpp/galay-kernel/core/task.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <iostream>
#include <thread>

namespace
{

using galay::kernel::Task;
using galay::kernel::detail::TaskResultError;
using galay::kernel::detail::TaskResultErrorCode;

static_assert(sizeof(galay::kernel::TaskState) <= 128,
              "TaskState must not grow beyond one cache line");

Task<int> value_task()
{
    co_return 42;
}

bool verify_error_delivery()
{
    constexpr std::array error_codes{
        TaskResultErrorCode::kInvalidState,
        TaskResultErrorCode::kTaskException,
        TaskResultErrorCode::kAlreadyConsumed,
        TaskResultErrorCode::kResultUnavailable,
        TaskResultErrorCode::kScheduleFailed,
        TaskResultErrorCode::kResumeFailed,
    };

    for (const auto code : error_codes) {
        Task<int> task = value_task();
        const auto reference = galay::kernel::detail::TaskAccess::task_ref(task);
        if (!reference.is_valid() || !reference.state()->m_handle) {
            std::cerr << "[T208] failed to allocate error-delivery task\n";
            return false;
        }

        galay::kernel::detail::store_task_error(reference, TaskResultError(code));
        reference.state()->m_handle.resume();
        const auto result =
            galay::kernel::detail::try_take_task_result<int>(reference);
        if (result || result.error().code() != code) {
            std::cerr << "[T208] result error code changed: expected "
                      << static_cast<unsigned>(code) << '\n';
            return false;
        }
    }
    return true;
}

bool verify_concurrent_single_consumption()
{
    constexpr std::size_t kConsumers = 8;
    for (std::size_t round = 0; round < 512; ++round) {
        Task<int> task = value_task();
        const auto reference = galay::kernel::detail::TaskAccess::task_ref(task);
        if (!reference.is_valid() || !reference.state()->m_handle) {
            std::cerr << "[T208] failed to allocate consumption task\n";
            return false;
        }
        reference.state()->m_handle.resume();

        std::atomic<bool> start{false};
        std::atomic<std::size_t> ready{0};
        std::atomic<std::size_t> consumed{0};
        std::atomic<std::size_t> already_consumed{0};
        std::atomic<bool> valid{true};
        std::array<std::thread, kConsumers> consumers;
        for (std::size_t index = 0; index < consumers.size(); ++index) {
            consumers[index] = std::thread([&, owner = reference]() {
                ready.fetch_add(1, std::memory_order_release);
                while (!start.load(std::memory_order_acquire)) {
                    std::this_thread::yield();
                }

                const auto result =
                    galay::kernel::detail::try_take_task_result<int>(owner);
                if (result) {
                    if (result.value() != 42) {
                        valid.store(false, std::memory_order_release);
                    }
                    consumed.fetch_add(1, std::memory_order_release);
                } else if (result.error().code() ==
                           TaskResultErrorCode::kAlreadyConsumed) {
                    already_consumed.fetch_add(1, std::memory_order_release);
                } else {
                    valid.store(false, std::memory_order_release);
                }
            });
        }
        while (ready.load(std::memory_order_acquire) != kConsumers) {
            std::this_thread::yield();
        }
        start.store(true, std::memory_order_release);
        for (auto& consumer : consumers) {
            consumer.join();
        }

        if (!valid.load(std::memory_order_acquire) ||
            consumed.load(std::memory_order_acquire) != 1 ||
            already_consumed.load(std::memory_order_acquire) != kConsumers - 1) {
            std::cerr << "[T208] concurrent result consumption was not single-shot\n";
            return false;
        }
    }
    return true;
}

} // namespace

int main()
{
    if (!verify_error_delivery() || !verify_concurrent_single_consumption()) {
        return 1;
    }
    std::cout << "T208-TaskResultState PASS\n";
    return 0;
}
