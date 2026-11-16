/**
 * @file t206_blocking_completion.cc
 * @brief 用途：验证阻塞任务完成态的跨线程等待与单次消费协议。
 * 关键覆盖点：并发等待、完成发布、单次结果消费、错误传递和 void 完成态。
 * 通过条件：所有等待者在完成发布后返回，且并发 take() 只有一个成功。
 */

#include <galay/cpp/galay-kernel/core/task.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const char* message)
{
    if (condition) return;
    if (std::fprintf(stderr, "%s\n", message) < 0) std::abort();
    std::exit(1);
}

void verify_concurrent_take_publishes_one_result()
{
    using namespace galay::kernel;
    constexpr int kWaiterCount = 8;
    constexpr int kIterationCount = 64;

    for (int iteration = 0; iteration < kIterationCount; ++iteration) {
        TaskCompletionState<std::array<int, 16>> completion;
        std::atomic<int> started{0};
        std::atomic<int> success_count{0};
        std::atomic<int> consumed_count{0};
        std::vector<std::thread> waiters;
        waiters.reserve(kWaiterCount);
        for (int index = 0; index < kWaiterCount; ++index) {
            auto& waiter = waiters.emplace_back([&]() {
                started.fetch_add(1, std::memory_order_release);
                auto result = completion.take();
                if (result.has_value()) {
                    require(result->front() == 42 && result->back() == 42,
                            "take must publish the completed value");
                    success_count.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                require(result.error().code() == detail::TaskResultErrorCode::kAlreadyConsumed,
                        "losing concurrent take must report already consumed");
                consumed_count.fetch_add(1, std::memory_order_relaxed);
            });
            require(waiter.joinable(), "completion waiter must be joinable");
        }

        while (started.load(std::memory_order_acquire) != kWaiterCount) {
            std::this_thread::yield();
        }
        if (iteration == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::array<int, 16> value{};
        value.fill(42);
        completion.set_value(value);
        for (auto& waiter : waiters) {
            waiter.join();
        }
        require(success_count.load(std::memory_order_relaxed) == 1,
                "exactly one concurrent take must succeed");
        require(consumed_count.load(std::memory_order_relaxed) == kWaiterCount - 1,
                "all losing takes must observe consumed state");
    }
}

void verify_shared_waits_wake_without_consuming()
{
    using namespace galay::kernel;
    constexpr int kWaiterCount = 8;
    TaskCompletionState<void> completion;
    std::atomic<int> started{0};
    std::vector<std::thread> waiters;
    waiters.reserve(kWaiterCount);
    for (int index = 0; index < kWaiterCount; ++index) {
        auto& waiter = waiters.emplace_back([&]() {
            started.fetch_add(1, std::memory_order_release);
            completion.wait();
            completion.wait();
        });
        require(waiter.joinable(), "shared wait completion must be joinable");
    }
    while (started.load(std::memory_order_acquire) != kWaiterCount) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    completion.set_value();
    for (auto& waiter : waiters) {
        waiter.join();
    }
    require(completion.take().has_value(), "shared waits must preserve completion for take");
}

void verify_error_notifies_blocked_take()
{
    using namespace galay::kernel;
    TaskCompletionState<int> completion;
    std::atomic<bool> started{false};
    std::thread waiter([&]() {
        started.store(true, std::memory_order_release);
        auto result = completion.take();
        require(!result.has_value() &&
                    result.error().code() == detail::TaskResultErrorCode::kTaskException,
                "blocked take must observe the published error");
    });
    while (!started.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    completion.set_error(detail::TaskResultError(detail::TaskResultErrorCode::kTaskException));
    waiter.join();
}

void verify_wait_repeat_and_consumption()
{
    using namespace galay::kernel;
    TaskCompletionState<int> completion;
    completion.set_value(17);
    completion.wait();
    completion.wait();

    auto first = completion.take();
    require(first.has_value() && *first == 17, "wait must preserve the completed value");
    auto repeated = completion.take();
    require(!repeated.has_value() &&
                repeated.error().code() == detail::TaskResultErrorCode::kAlreadyConsumed,
            "repeated take must report already consumed");
}

void verify_error_and_void_completion()
{
    using namespace galay::kernel;
    TaskCompletionState<int> failed;
    failed.set_error(detail::TaskResultError(detail::TaskResultErrorCode::kTaskException));
    auto error_result = failed.take();
    require(!error_result.has_value() &&
                error_result.error().code() == detail::TaskResultErrorCode::kTaskException,
            "error completion must reach the consumer");

    TaskCompletionState<void> completed;
    completed.set_value();
    completed.wait();
    require(completed.take().has_value(), "void completion must succeed");
    auto repeated = completed.take();
    require(!repeated.has_value() &&
                repeated.error().code() == detail::TaskResultErrorCode::kAlreadyConsumed,
            "repeated void take must report already consumed");
}

}  // namespace

int main()
{
    verify_concurrent_take_publishes_one_result();
    verify_shared_waits_wake_without_consuming();
    verify_error_notifies_blocked_take();
    verify_wait_repeat_and_consumption();
    verify_error_and_void_completion();
    require(std::puts("T206-BlockingCompletion PASS") >= 0, "write result");
    return 0;
}
