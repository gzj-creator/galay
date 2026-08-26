/**
 * @file t180_coroutine_frame_allocator.cc
 * @brief 协程 frame 分配器、Task 生命周期和跨线程释放边界测试。
 */

#include <galay/cpp/galay-kernel/core/runtime.h>
#include <galay/cpp/galay-kernel/core/task.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <coroutine>
#include <cstdint>
#include <iostream>
#include <limits>
#include <thread>
#include <utility>

using galay::kernel::Runtime;
using galay::kernel::RuntimeConfig;
using galay::kernel::Task;
using galay::kernel::TaskPromise;
using galay::kernel::TaskRef;
using galay::kernel::JoinHandle;
using galay::kernel::detail::allocateFrameStorage;
using galay::kernel::detail::destroyTaskFrame;
using galay::kernel::detail::releaseFrameStorage;

namespace {

constexpr std::size_t kDefaultAlignment = alignof(std::max_align_t);
constexpr std::size_t kFrameCachePressureCount = 257;

bool require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "[T180] " << message << '\n';
        return false;
    }
    return true;
}

Task<int> completedIntTask() {
    co_return 7;
}

Task<void> completedVoidTask() {
    co_return;
}

Task<int> nestedChildTask() {
    co_return 5;
}

Task<int> nestedParentTask() {
    auto result = co_await nestedChildTask();
    if (!result.has_value()) {
        std::cerr << "[T180] nested child error="
                  << static_cast<int>(result.error().code()) << '\n';
        co_return -1;
    }
    co_return *result + 1;
}

Task<int> unhandledExceptionTask() {
    throw 42;
    co_return 0;
}

struct FrameProbe {
    std::atomic<int>* destroyed = nullptr;
    bool countOnDestroy = false;

    explicit FrameProbe(std::atomic<int>* target)
        : destroyed(target) {}

    FrameProbe(const FrameProbe& other)
        : destroyed(other.destroyed), countOnDestroy(true) {}

    FrameProbe(FrameProbe&& other) noexcept
        : destroyed(other.destroyed), countOnDestroy(true) {
        other.countOnDestroy = false;
    }

    ~FrameProbe() {
        if (countOnDestroy) {
            destroyed->fetch_add(1, std::memory_order_release);
        }
    }
};

Task<void> initiallySuspendedTask(FrameProbe probe) {
    co_await std::suspend_always{};
}

bool verifySmallAndOverflowInputs() {
    releaseFrameStorage(nullptr, 0, 0);

    void* zero = allocateFrameStorage(0, 0);
    if (!require(zero != nullptr, "zero-sized frame allocation failed")) {
        return false;
    }
    releaseFrameStorage(zero, 0, 0);

    void* one = allocateFrameStorage(1, 1);
    if (!require(one != nullptr,
                 "one-byte frame allocation failed after unsized release")) {
        if (one != nullptr) {
            releaseFrameStorage(one, 1, 1);
        }
        return false;
    }
    releaseFrameStorage(one, 1, 1);

    const auto max = std::numeric_limits<std::size_t>::max();
    if (!require(allocateFrameStorage(max, kDefaultAlignment) == nullptr,
                 "overflow-sized frame must fail without allocation")) {
        return false;
    }
    if (!require(allocateFrameStorage(128, max) == nullptr,
                 "overflow alignment must fail without allocation")) {
        return false;
    }

    for (const std::size_t alignment : {0UL, 1UL, 2UL, 8UL, kDefaultAlignment}) {
        void* frame = allocateFrameStorage(64, alignment);
        if (!require(frame != nullptr, "small alignment allocation failed")) {
            return false;
        }
        if (!require(reinterpret_cast<std::uintptr_t>(frame) % kDefaultAlignment == 0,
                     "small alignment must preserve max_align_t alignment")) {
            releaseFrameStorage(frame, 64, alignment);
            return false;
        }
        releaseFrameStorage(frame, 64, alignment);
    }
    return true;
}

bool verifyBoundaryReuse() {
    constexpr std::array<std::pair<std::size_t, std::size_t>, 5> boundaries{{
        {127, 128},
        {255, 256},
        {511, 512},
        {1023, 1024},
        {2047, 2048},
    }};

    for (const auto [lower, upper] : boundaries) {
        void* first = allocateFrameStorage(lower, kDefaultAlignment);
        if (!require(first != nullptr, "boundary allocation failed")) {
            return false;
        }
        releaseFrameStorage(first, lower, kDefaultAlignment);

        void* sameBucket = allocateFrameStorage(upper, kDefaultAlignment);
        if (!require(sameBucket == first,
                     "adjacent sizes should reuse one frame bucket")) {
            if (sameBucket != nullptr) {
                releaseFrameStorage(sameBucket, upper, kDefaultAlignment);
            }
            return false;
        }
        releaseFrameStorage(sameBucket, upper, kDefaultAlignment);

        void* nextBucket = allocateFrameStorage(upper + 1, kDefaultAlignment);
        if (!require(nextBucket != nullptr && nextBucket != first,
                     "next frame bucket should not reuse a smaller block")) {
            if (nextBucket != nullptr) {
                releaseFrameStorage(nextBucket, upper + 1, kDefaultAlignment);
            }
            return false;
        }
        releaseFrameStorage(nextBucket, upper + 1, kDefaultAlignment);
    }
    return true;
}

bool verifyCapacityAndFallback() {
    std::array<void*, kFrameCachePressureCount> nodes{};
    for (void*& node : nodes) {
        node = allocateFrameStorage(128, kDefaultAlignment);
        if (!require(node != nullptr, "capacity test allocation failed")) {
            return false;
        }
    }
    for (void* node : nodes) {
        releaseFrameStorage(node, 128, kDefaultAlignment);
    }
    void* large = allocateFrameStorage(2049, kDefaultAlignment);
    if (!require(large != nullptr, "large frame fallback allocation failed")) {
        return false;
    }
    releaseFrameStorage(large, 2049, kDefaultAlignment);

    constexpr std::size_t kOverAlignment = kDefaultAlignment * 2;
    void* overAligned = allocateFrameStorage(128, kOverAlignment);
    if (!require(overAligned != nullptr,
                 "over-aligned frame fallback allocation failed")) {
        return false;
    }
    if (!require(reinterpret_cast<std::uintptr_t>(overAligned) % kOverAlignment == 0,
                 "over-aligned frame has incorrect alignment")) {
        releaseFrameStorage(overAligned, 128, kOverAlignment);
        return false;
    }
    releaseFrameStorage(overAligned, 128, kOverAlignment);
    return true;
}

bool verifyDeleteCombinations() {
    static_assert(noexcept(TaskPromise<int>::operator new(std::size_t{})));
    static_assert(noexcept(TaskPromise<void>::operator new(
        std::size_t{}, std::align_val_t{})));

    void* ordinary = TaskPromise<void>::operator new(128);
    if (!require(ordinary != nullptr, "ordinary promise allocation failed")) {
        return false;
    }
    TaskPromise<void>::operator delete(ordinary);

    void* sized = TaskPromise<int>::operator new(128);
    if (!require(sized != nullptr, "sized promise allocation failed")) {
        return false;
    }
    TaskPromise<int>::operator delete(sized, 128);

    const auto alignment = std::align_val_t(kDefaultAlignment);
    void* aligned = TaskPromise<void>::operator new(128, alignment);
    if (!require(aligned != nullptr, "aligned promise allocation failed")) {
        return false;
    }
    TaskPromise<void>::operator delete(aligned, alignment);

    void* sizedAligned = TaskPromise<int>::operator new(128, alignment);
    if (!require(sizedAligned != nullptr,
                 "sized+aligned promise allocation failed")) {
        return false;
    }
    TaskPromise<int>::operator delete(sizedAligned, 128, alignment);

    void* nothrow = TaskPromise<void>::operator new(128, std::nothrow);
    if (!require(nothrow != nullptr, "nothrow promise allocation failed")) {
        return false;
    }
    TaskPromise<void>::operator delete(nothrow);

    const auto overAlignment = std::align_val_t(kDefaultAlignment * 2);
    void* nothrowAligned =
        TaskPromise<int>::operator new(128, overAlignment, std::nothrow);
    if (!require(nothrowAligned != nullptr,
                 "aligned nothrow promise allocation failed")) {
        return false;
    }
    if (!require(reinterpret_cast<std::uintptr_t>(nothrowAligned) %
                     static_cast<std::size_t>(overAlignment) ==
                     0,
                 "aligned nothrow promise has incorrect alignment")) {
        TaskPromise<int>::operator delete(nothrowAligned, overAlignment);
        return false;
    }
    TaskPromise<int>::operator delete(nothrowAligned, overAlignment);

    void* unsized = TaskPromise<void>::operator new(128);
    if (!require(unsized != nullptr, "unsized promise allocation failed")) {
        return false;
    }
    TaskPromise<void>::operator delete(unsized);
    return true;
}

bool verifyAllocationFailureContract() {
    if (!require(!TaskPromise<int>::get_return_object_on_allocation_failure().isValid(),
                 "Task<int> allocation failure must return an invalid task")) {
        return false;
    }
    if (!require(!TaskPromise<void>::get_return_object_on_allocation_failure().isValid(),
                 "Task<void> allocation failure must return an invalid task")) {
        return false;
    }

    return true;
}

bool verifyConcurrentRawFrameChurn() {
    constexpr std::size_t kWorkers = 4;
    constexpr std::size_t kIterations = 25'000;
    std::atomic<std::size_t> failures{0};
    std::atomic<std::size_t> badAlignment{0};
    std::array<std::thread, kWorkers> workers;

    for (auto& worker : workers) {
        worker = std::thread([&]() {
            for (std::size_t i = 0; i < kIterations; ++i) {
                const std::size_t size =
                    (i % 2 == 0) ? 256 : ((i % 3 == 0) ? 512 : 2048);
                void* frame = allocateFrameStorage(size, kDefaultAlignment);
                if (frame == nullptr) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                if (reinterpret_cast<std::uintptr_t>(frame) % kDefaultAlignment != 0) {
                    badAlignment.fetch_add(1, std::memory_order_relaxed);
                }
                releaseFrameStorage(frame, size, kDefaultAlignment);
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    return require(failures.load(std::memory_order_relaxed) == 0,
                   "concurrent frame churn must not fail allocations") &&
        require(badAlignment.load(std::memory_order_relaxed) == 0,
                "concurrent frame churn returned an invalid alignment");
}

bool verifyCrossThreadRawFrameRelease() {
    constexpr std::size_t kFrames = 1024;
    std::array<void*, kFrames> frames{};
    std::thread producer([&]() {
        for (void*& frame : frames) {
            frame = allocateFrameStorage(256, kDefaultAlignment);
        }
    });
    producer.join();

    std::thread consumer([&]() {
        for (void* frame : frames) {
            releaseFrameStorage(frame, 256, kDefaultAlignment);
        }
    });
    consumer.join();
    return true;
}

bool verifyBasicTaskLifetimes() {
    {
        auto task = completedIntTask();
        if (!require(task.isValid(), "Task<int> should be valid after creation")) {
            return false;
        }
        TaskRef keeper = galay::kernel::detail::TaskAccess::taskRef(task);
        auto* state = keeper.state();
        if (!require(state != nullptr && state->m_handle != nullptr,
                     "Task<int> should expose a coroutine handle")) {
            return false;
        }
        state->m_handle.resume();
        if (!require(state->m_done.load(std::memory_order_acquire),
                     "Task<int> should be complete after resume")) {
            return false;
        }
        auto result = galay::kernel::detail::TaskAccess::takeResult(task);
        if (!require(result.has_value() && *result == 7,
                     "Task<int> result should be consumable once")) {
            return false;
        }
        auto consumedAgain = galay::kernel::detail::TaskAccess::takeResult(task);
        if (!require(!consumedAgain.has_value() &&
                         consumedAgain.error().code() ==
                             galay::kernel::detail::TaskResultErrorCode::kAlreadyConsumed,
                     "Task<int> second result consumption must fail explicitly")) {
            return false;
        }
        if (!require(keeper.isValid() && keeper.state() == state,
                     "TaskState must outlive its coroutine frame")) {
            return false;
        }
    }

    auto voidTask = completedVoidTask();
    if (!require(voidTask.isValid(), "Task<void> should be valid after creation")) {
        return false;
    }
    auto* voidState = galay::kernel::detail::TaskAccess::taskRef(voidTask).state();
    voidState->m_handle.resume();
    auto voidResult = galay::kernel::detail::TaskAccess::takeResult(voidTask);
    return require(voidResult.has_value(), "Task<void> should complete normally");
}

bool verifyUnsubmittedAndCrossThreadDestroy() {
    std::atomic<int> destroyed{0};
    {
        auto task = initiallySuspendedTask(FrameProbe(&destroyed));
        if (!require(task.isValid(), "initially suspended task should be valid")) {
            return false;
        }
    }
    if (!require(destroyed.load(std::memory_order_acquire) == 1,
                 "dropping an unsubmitted task must destroy its frame once")) {
        return false;
    }

    destroyed.store(0, std::memory_order_release);
    {
        auto task = initiallySuspendedTask(FrameProbe(&destroyed));
        std::thread releaser([task = std::move(task)]() mutable {
            (void)task;
        });
        releaser.join();
    }
    return require(destroyed.load(std::memory_order_acquire) == 1,
                   "cross-thread frame release must destroy exactly once");
}

bool verifyExplicitDestroyGuard() {
    std::atomic<int> destroyed{0};
    auto task = initiallySuspendedTask(FrameProbe(&destroyed));
    TaskRef keeper = galay::kernel::detail::TaskAccess::taskRef(task);
    auto* state = keeper.state();
    task = Task<void>{};
    if (!require(destroyTaskFrame(state), "first explicit frame destroy should succeed")) {
        return false;
    }
    if (!require(!destroyTaskFrame(state), "repeated frame destroy should be ignored")) {
        return false;
    }
    keeper = TaskRef{};
    return require(destroyed.load(std::memory_order_acquire) == 1,
                   "explicit frame destruction should run its destructor once");
}

bool verifyDirectHandleDestroyGuard() {
    std::atomic<int> destroyed{0};
    auto task = initiallySuspendedTask(FrameProbe(&destroyed));
    TaskRef keeper = galay::kernel::detail::TaskAccess::taskRef(task);
    auto* state = keeper.state();
    task = Task<void>{};

    auto handle = state->m_handle;
    handle.destroy();
    if (!require(state->m_handle == nullptr,
                 "promise destruction must clear a directly destroyed frame handle")) {
        keeper = TaskRef{};
        return false;
    }
    keeper = TaskRef{};
    return require(destroyed.load(std::memory_order_acquire) == 1,
                   "direct frame destroy must not be repeated by TaskState");
}

bool verifyStateRetentionHandles() {
    {
        auto task = completedIntTask();
        TaskRef taskRef = galay::kernel::detail::TaskAccess::taskRef(task);
        auto* state = taskRef.state();
        JoinHandle<int> join(taskRef);
        state->m_handle.resume();
        if (!require(state->m_done.load(std::memory_order_acquire),
                     "join retention task should complete before Task release")) {
            return false;
        }
        if (!require(state->m_handle == nullptr,
                     "completed frame should be destroyed before JoinHandle release")) {
            return false;
        }
        task = Task<int>{};
        auto result = join.join();
        if (!require(result.has_value() && *result == 7,
                     "JoinHandle must retain completed TaskState")) {
            return false;
        }
    }

    std::atomic<int> destroyed{0};
    {
        auto root = completedVoidTask();
        auto next = initiallySuspendedTask(FrameProbe(&destroyed));
        auto* rootState = galay::kernel::detail::TaskAccess::taskRef(root).state();
        root.then(std::move(next));
        rootState->m_handle.resume();
        root = Task<void>{};
    }
    return require(destroyed.load(std::memory_order_acquire) == 1,
                   "continuation must retain and then release its child frame once");
}

bool verifyNestedAndExceptionTasks() {
    {
        auto child = nestedChildTask();
        auto childRef = galay::kernel::detail::TaskAccess::taskRef(child);
        auto* childState = childRef.state();
        childState->m_handle.resume();
        auto childResult = galay::kernel::detail::TaskAccess::takeResult(child);
        if (!require(childResult.has_value() && *childResult == 5,
                     "direct child task should produce a result")) {
            return false;
        }
    }

    RuntimeConfig config;
    config.io_scheduler_count = 0;
    config.parallel_scheduler_count = 1;
    Runtime runtime(config);

    auto nested = runtime.blockOnCpu(nestedParentTask());
    if (!require(nested.has_value() && *nested == 6,
                 "nested co_await should preserve result and lifetime")) {
        if (nested.has_value()) {
            std::cerr << "[T180] nested value=" << *nested << '\n';
        } else {
            std::cerr << "[T180] nested error="
                      << static_cast<int>(nested.error().code()) << '\n';
        }
        runtime.stop();
        return false;
    }

    auto exception = runtime.blockOnCpu(unhandledExceptionTask());
    if (!require(!exception.has_value(),
                 "unhandled_exception should produce a failed root task")) {
        runtime.stop();
        return false;
    }
    runtime.stop();
    return require(exception.error().code() ==
                        galay::kernel::RuntimeErrorCode::kTaskException,
                    "unhandled_exception should preserve its error category");
}

}  // namespace

int main() {
    if (!verifySmallAndOverflowInputs() ||
        !verifyBoundaryReuse() ||
        !verifyCapacityAndFallback() ||
        !verifyDeleteCombinations() ||
        !verifyAllocationFailureContract() ||
        !verifyConcurrentRawFrameChurn() ||
        !verifyCrossThreadRawFrameRelease() ||
        !verifyBasicTaskLifetimes() ||
        !verifyUnsubmittedAndCrossThreadDestroy() ||
        !verifyExplicitDestroyGuard() ||
        !verifyDirectHandleDestroyGuard() ||
        !verifyStateRetentionHandles() ||
        !verifyNestedAndExceptionTasks()) {
        return 1;
    }

    std::cout << "T180-CoroutineFrameAllocator PASS\n";
    return 0;
}
