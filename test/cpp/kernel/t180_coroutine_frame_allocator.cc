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
#include <new>
#include <thread>
#include <utility>

using galay::kernel::Runtime;
using galay::kernel::RuntimeConfig;
using galay::kernel::Task;
using galay::kernel::TaskPromise;
using galay::kernel::TaskRef;
using galay::kernel::JoinHandle;
using galay::kernel::detail::allocate_frame_storage;
using galay::kernel::detail::destroy_task_frame;
using galay::kernel::detail::frame_free_list_size_for_testing;
using galay::kernel::detail::release_frame_storage;
using galay::kernel::detail::set_frame_allocation_failure_for_testing;
using galay::kernel::detail::set_frame_recycler_enabled_for_testing;
using galay::kernel::detail::set_task_state_allocation_failure_for_testing;

namespace {

constexpr std::size_t kDefaultAlignment = alignof(std::max_align_t);
constexpr std::size_t kFrameCachePressureCount = 257;
constexpr std::size_t kFrameCacheLimit = 256;

bool require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "[T180] " << message << '\n';
        return false;
    }
    return true;
}

Task<int> completed_int_task() {
    co_return 7;
}

Task<void> completed_void_task() {
    co_return;
}

Task<int> nested_child_task() {
    co_return 5;
}

Task<int> nested_parent_task() {
    auto result = co_await nested_child_task();
    if (!result.has_value()) {
        std::cerr << "[T180] nested child error="
                  << static_cast<int>(result.error().code()) << '\n';
        co_return -1;
    }
    co_return *result + 1;
}

Task<int> unhandled_exception_task() {
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

Task<void> initially_suspended_task(FrameProbe probe) {
    co_await std::suspend_always{};
}

bool verify_small_and_overflow_inputs() {
    release_frame_storage(nullptr, 0, 0);

    void* zero = allocate_frame_storage(0, 0);
    if (!require(zero != nullptr, "zero-sized frame allocation failed")) {
        return false;
    }
    release_frame_storage(zero, 0, 0);

    void* one = allocate_frame_storage(1, 1);
    if (!require(one != nullptr,
                 "one-byte frame allocation failed after unsized release")) {
        if (one != nullptr) {
            release_frame_storage(one, 1, 1);
        }
        return false;
    }
    release_frame_storage(one, 1, 1);

    const auto max = std::numeric_limits<std::size_t>::max();
    if (!require(allocate_frame_storage(max, kDefaultAlignment) == nullptr,
                 "overflow-sized frame must fail without allocation")) {
        return false;
    }
    if (!require(allocate_frame_storage(128, max) == nullptr,
                 "overflow alignment must fail without allocation")) {
        return false;
    }

    for (const std::size_t alignment : {0UL, 1UL, 2UL, 8UL, kDefaultAlignment}) {
        void* frame = allocate_frame_storage(64, alignment);
        if (!require(frame != nullptr, "small alignment allocation failed")) {
            return false;
        }
        if (!require(reinterpret_cast<std::uintptr_t>(frame) % kDefaultAlignment == 0,
                     "small alignment must preserve max_align_t alignment")) {
            release_frame_storage(frame, 64, alignment);
            return false;
        }
        release_frame_storage(frame, 64, alignment);
    }
    return true;
}

bool verify_boundary_reuse() {
    constexpr std::array<std::pair<std::size_t, std::size_t>, 5> boundaries{{
        {127, 128},
        {255, 256},
        {511, 512},
        {1023, 1024},
        {2047, 2048},
    }};

    for (const auto [lower, upper] : boundaries) {
        void* first = allocate_frame_storage(lower, kDefaultAlignment);
        if (!require(first != nullptr, "boundary allocation failed")) {
            return false;
        }
        release_frame_storage(first, lower, kDefaultAlignment);

        void* sameBucket = allocate_frame_storage(upper, kDefaultAlignment);
        if (!require(sameBucket == first,
                     "adjacent sizes should reuse one frame bucket")) {
            if (sameBucket != nullptr) {
                release_frame_storage(sameBucket, upper, kDefaultAlignment);
            }
            return false;
        }
        release_frame_storage(sameBucket, upper, kDefaultAlignment);

        void* nextBucket = allocate_frame_storage(upper + 1, kDefaultAlignment);
        if (!require(nextBucket != nullptr && nextBucket != first,
                     "next frame bucket should not reuse a smaller block")) {
            if (nextBucket != nullptr) {
                release_frame_storage(nextBucket, upper + 1, kDefaultAlignment);
            }
            return false;
        }
        release_frame_storage(nextBucket, upper + 1, kDefaultAlignment);
    }
    return true;
}

bool verify_capacity_and_fallback() {
    // 先清空 128 字节缓存桶，使容量断言不受前面边界检查分配结果的影响。
    const auto cached = frame_free_list_size_for_testing(128, kDefaultAlignment);
    std::array<void*, kFrameCacheLimit> drained{};
    for (std::size_t i = 0; i < cached; ++i) {
        drained[i] = allocate_frame_storage(128, kDefaultAlignment);
        if (!require(drained[i] != nullptr, "failed to drain frame bucket")) {
            return false;
        }
    }
    set_frame_recycler_enabled_for_testing(false);
    for (std::size_t i = 0; i < cached; ++i) {
        release_frame_storage(drained[i], 128, kDefaultAlignment);
    }
    set_frame_recycler_enabled_for_testing(true);

    std::array<void*, kFrameCachePressureCount> nodes{};
    for (void*& node : nodes) {
        node = allocate_frame_storage(128, kDefaultAlignment);
        if (!require(node != nullptr, "capacity test allocation failed")) {
            return false;
        }
    }
    for (void* node : nodes) {
        release_frame_storage(node, 128, kDefaultAlignment);
    }
    if (!require(frame_free_list_size_for_testing(128, kDefaultAlignment) == kFrameCacheLimit,
                 "frame bucket must cap at 256 nodes")) {
        return false;
    }

    // 过期的编译器 sized-delete 参数不能把内存块放入更大的缓存桶。
    // 分配头才是判断真实来源的依据。
    void* small = allocate_frame_storage(128, kDefaultAlignment);
    if (!require(small != nullptr, "wrong-size provenance allocation failed")) {
        return false;
    }
    release_frame_storage(small, 200, kDefaultAlignment);
    void* reused = allocate_frame_storage(128, kDefaultAlignment);
    if (!require(reused == small,
                 "mismatched release size must preserve the original bucket")) {
        if (reused != nullptr) {
            release_frame_storage(reused, 128, kDefaultAlignment);
        }
        return false;
    }
    release_frame_storage(reused, 128, kDefaultAlignment);

    const auto cachedBeforeLarge =
        frame_free_list_size_for_testing(2048, kDefaultAlignment);
    void* large = allocate_frame_storage(2049, kDefaultAlignment);
    if (!require(large != nullptr, "large frame fallback allocation failed")) {
        return false;
    }
    release_frame_storage(large, 2049, kDefaultAlignment);
    if (!require(frame_free_list_size_for_testing(2048, kDefaultAlignment) ==
                     cachedBeforeLarge,
                 "large frames must bypass TLS buckets")) {
        return false;
    }

    constexpr std::size_t kOverAlignment = kDefaultAlignment * 2;
    void* overAligned = allocate_frame_storage(128, kOverAlignment);
    if (!require(overAligned != nullptr,
                 "over-aligned frame fallback allocation failed")) {
        return false;
    }
    if (!require(reinterpret_cast<std::uintptr_t>(overAligned) % kOverAlignment == 0,
                 "over-aligned frame has incorrect alignment")) {
        release_frame_storage(overAligned, 128, kOverAlignment);
        return false;
    }
    release_frame_storage(overAligned, 128, kOverAlignment);
    if (!require(frame_free_list_size_for_testing(128, kDefaultAlignment) ==
                     kFrameCacheLimit,
                 "over-aligned frames must bypass ordinary buckets")) {
        return false;
    }
    return true;
}

bool verify_delete_combinations() {
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

bool verify_allocation_failure_contract() {
    if (!require(!TaskPromise<int>::get_return_object_on_allocation_failure().is_valid(),
                 "Task<int> allocation failure must return an invalid task")) {
        return false;
    }
    if (!require(!TaskPromise<void>::get_return_object_on_allocation_failure().is_valid(),
                 "Task<void> allocation failure must return an invalid task")) {
        return false;
    }

    set_frame_allocation_failure_for_testing(true);
    auto noFrame = completed_int_task();
    set_frame_allocation_failure_for_testing(false);
    if (!require(!noFrame.is_valid(),
                 "frame allocation failure must return an invalid Task")) {
        return false;
    }

    set_task_state_allocation_failure_for_testing(true);
    auto noState = completed_int_task();
    set_task_state_allocation_failure_for_testing(false);
    if (!require(!noState.is_valid(),
                 "TaskState allocation failure must return an invalid Task")) {
        return false;
    }

    std::atomic<int> destroyed{0};
    set_task_state_allocation_failure_for_testing(true);
    auto noStateFrame = initially_suspended_task(FrameProbe(&destroyed));
    set_task_state_allocation_failure_for_testing(false);
    if (!require(!noStateFrame.is_valid(),
                 "initial suspend must return an invalid Task when state allocation fails") ||
        !require(destroyed.load(std::memory_order_acquire) == 1,
                 "initial suspend must destroy its frame after state allocation failure")) {
        return false;
    }

    return true;
}

bool verify_concurrent_task_state_churn() {
    constexpr std::size_t kWorkers = 4;
    constexpr std::size_t kIterations = 20'000;
    std::atomic<std::size_t> failures{0};
    std::array<std::thread, kWorkers> workers;

    for (auto& worker : workers) {
        worker = std::thread([&]() {
            for (std::size_t i = 0; i < kIterations; ++i) {
                auto* state = new (std::nothrow) galay::kernel::TaskState(
                    std::coroutine_handle<>{});
                if (state == nullptr) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                delete state;
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    return require(failures.load(std::memory_order_relaxed) == 0,
                   "concurrent TaskState allocation must not fail");
}

bool verify_concurrent_raw_frame_churn() {
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
                void* frame = allocate_frame_storage(size, kDefaultAlignment);
                if (frame == nullptr) {
                    failures.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }
                if (reinterpret_cast<std::uintptr_t>(frame) % kDefaultAlignment != 0) {
                    badAlignment.fetch_add(1, std::memory_order_relaxed);
                }
                release_frame_storage(frame, size, kDefaultAlignment);
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

bool verify_cross_thread_raw_frame_release() {
    constexpr std::size_t kFrames = 1024;
    std::array<void*, kFrames> frames{};
    std::thread producer([&]() {
        for (void*& frame : frames) {
            frame = allocate_frame_storage(256, kDefaultAlignment);
        }
    });
    producer.join();

    std::thread consumer([&]() {
        for (void* frame : frames) {
            release_frame_storage(frame, 256, kDefaultAlignment);
        }
    });
    consumer.join();
    return true;
}

bool verify_basic_task_lifetimes() {
    {
        auto task = completed_int_task();
        if (!require(task.is_valid(), "Task<int> should be valid after creation")) {
            return false;
        }
        TaskRef keeper = galay::kernel::detail::TaskAccess::task_ref(task);
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
        auto result = galay::kernel::detail::TaskAccess::take_result(task);
        if (!require(result.has_value() && *result == 7,
                     "Task<int> result should be consumable once")) {
            return false;
        }
        auto consumedAgain = galay::kernel::detail::TaskAccess::take_result(task);
        if (!require(!consumedAgain.has_value() &&
                         consumedAgain.error().code() ==
                             galay::kernel::detail::TaskResultErrorCode::kAlreadyConsumed,
                     "Task<int> second result consumption must fail explicitly")) {
            return false;
        }
        if (!require(keeper.is_valid() && keeper.state() == state,
                     "TaskState must outlive its coroutine frame")) {
            return false;
        }
    }

    auto voidTask = completed_void_task();
    if (!require(voidTask.is_valid(), "Task<void> should be valid after creation")) {
        return false;
    }
    auto* voidState = galay::kernel::detail::TaskAccess::task_ref(voidTask).state();
    voidState->m_handle.resume();
    auto voidResult = galay::kernel::detail::TaskAccess::take_result(voidTask);
    return require(voidResult.has_value(), "Task<void> should complete normally");
}

bool verify_unsubmitted_and_cross_thread_destroy() {
    std::atomic<int> destroyed{0};
    {
        auto task = initially_suspended_task(FrameProbe(&destroyed));
        if (!require(task.is_valid(), "initially suspended task should be valid")) {
            return false;
        }
    }
    if (!require(destroyed.load(std::memory_order_acquire) == 1,
                 "dropping an unsubmitted task must destroy its frame once")) {
        return false;
    }

    destroyed.store(0, std::memory_order_release);
    {
        auto task = initially_suspended_task(FrameProbe(&destroyed));
        std::thread releaser([task = std::move(task)]() mutable {
            (void)task;
        });
        releaser.join();
    }
    return require(destroyed.load(std::memory_order_acquire) == 1,
                   "cross-thread frame release must destroy exactly once");
}

bool verify_explicit_destroy_guard() {
    std::atomic<int> destroyed{0};
    auto task = initially_suspended_task(FrameProbe(&destroyed));
    TaskRef keeper = galay::kernel::detail::TaskAccess::task_ref(task);
    auto* state = keeper.state();
    task = Task<void>{};
    if (!require(destroy_task_frame(state), "first explicit frame destroy should succeed")) {
        return false;
    }
    if (!require(!destroy_task_frame(state), "repeated frame destroy should be ignored")) {
        return false;
    }
    keeper = TaskRef{};
    return require(destroyed.load(std::memory_order_acquire) == 1,
                   "explicit frame destruction should run its destructor once");
}

bool verify_direct_handle_destroy_guard() {
    std::atomic<int> destroyed{0};
    auto task = initially_suspended_task(FrameProbe(&destroyed));
    TaskRef keeper = galay::kernel::detail::TaskAccess::task_ref(task);
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

bool verify_state_retention_handles() {
    {
        auto task = completed_int_task();
        TaskRef taskRef = galay::kernel::detail::TaskAccess::task_ref(task);
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
        auto root = completed_void_task();
        auto next = initially_suspended_task(FrameProbe(&destroyed));
        auto* rootState = galay::kernel::detail::TaskAccess::task_ref(root).state();
        root.then(std::move(next));
        rootState->m_handle.resume();
        root = Task<void>{};
    }
    return require(destroyed.load(std::memory_order_acquire) == 1,
                   "continuation must retain and then release its child frame once");
}

bool verify_nested_and_exception_tasks() {
    {
        auto child = nested_child_task();
        auto childRef = galay::kernel::detail::TaskAccess::task_ref(child);
        auto* childState = childRef.state();
        childState->m_handle.resume();
        auto childResult = galay::kernel::detail::TaskAccess::take_result(child);
        if (!require(childResult.has_value() && *childResult == 5,
                     "direct child task should produce a result")) {
            return false;
        }
    }

    RuntimeConfig config;
    config.io_scheduler_count = 0;
    config.parallel_scheduler_count = 1;
    Runtime runtime(config);

    auto nested = runtime.block_on_cpu(nested_parent_task());
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

    auto exception = runtime.block_on_cpu(unhandled_exception_task());
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

bool verify_fresh_thread_cache_lifecycle() {
    constexpr std::array<std::size_t, 5> sizes{{128, 256, 512, 1024, 2048}};
    for (size_t round = 0; round < 32; ++round) {
        std::array<void*, sizes.size()> frames{};
        for (size_t i = 0; i < sizes.size(); ++i) {
            frames[i] = allocate_frame_storage(sizes[i], kDefaultAlignment);
            if (frames[i] == nullptr) {
                for (size_t j = 0; j < i; ++j) {
                    release_frame_storage(frames[j], sizes[j], kDefaultAlignment);
                }
                return false;
            }
        }
        bool valid = true;
        // First allocator operation on this fresh thread is a remote release.
        std::thread consumer([&]() {
            for (size_t i = 0; i < sizes.size(); ++i) {
                release_frame_storage(frames[i], sizes[i], kDefaultAlignment);
                void* reused = allocate_frame_storage(sizes[i], kDefaultAlignment);
                if (reused != frames[i]) { valid = false; }
                release_frame_storage(reused, sizes[i], kDefaultAlignment);
                if (frame_free_list_size_for_testing(sizes[i], kDefaultAlignment) != 1) {
                    valid = false;
                }
            }
            // Thread exit must clean all five populated buckets.
        });
        consumer.join();
        if (!require(valid, "fresh-thread release/reuse changed bucket ownership")) {
            return false;
        }
    }
    return true;
}

}  // namespace

int main() {
    if (!verify_fresh_thread_cache_lifecycle() ||
        !verify_small_and_overflow_inputs() ||
        !verify_boundary_reuse() ||
        !verify_capacity_and_fallback() ||
        !verify_delete_combinations() ||
        !verify_allocation_failure_contract() ||
        !verify_concurrent_raw_frame_churn() ||
        !verify_cross_thread_raw_frame_release() ||
        !verify_basic_task_lifetimes() ||
        !verify_unsubmitted_and_cross_thread_destroy() ||
        !verify_explicit_destroy_guard() ||
        !verify_direct_handle_destroy_guard() ||
        !verify_state_retention_handles() ||
        !verify_concurrent_task_state_churn() ||
        !verify_nested_and_exception_tasks()) {
        return 1;
    }

    std::cout << "T180-CoroutineFrameAllocator PASS\n";
    return 0;
}
