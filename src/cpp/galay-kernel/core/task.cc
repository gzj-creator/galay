/**
 * @file task.cc
 * @brief 任务状态分配器、TaskRef 生命周期及任务调度辅助函数
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 实现：
 * - TaskState 对象的线程局部空闲链分配器
 * - TaskRef 引用计数（retain/release）
 * - TaskState 析构函数和完成状态通知
 * - 任务生命周期辅助函数：调度、完成、等待、continuation 附加
 * - 线程局部 Runtime 作用域管理（g_currentRuntime）
 */

#include "task.h"
#include "scheduler.hpp"
#include "scheduler_dispatch.hpp"

#include <limits>

namespace galay::kernel
{

namespace
{

thread_local Runtime* g_currentRuntime = nullptr;
struct TaskStateFreeNode
{
    TaskStateFreeNode* next = nullptr;
};

thread_local TaskStateFreeNode* g_taskStateFreeList = nullptr;
thread_local size_t g_taskStateFreeCount = 0;
// 让 TaskState 驻留量与 frame 缓存上限保持同量级。
constexpr size_t kTaskStateFreeListLimit = 256;

struct TaskStateFreeListCleanup
{
    ~TaskStateFreeListCleanup() noexcept
    {
        while (g_taskStateFreeList != nullptr) {
            auto* node = g_taskStateFreeList;
            g_taskStateFreeList = node->next;
            ::operator delete(node, std::align_val_t(alignof(TaskState)));
        }
        g_taskStateFreeCount = 0;
    }
};

thread_local TaskStateFreeListCleanup g_taskStateFreeListCleanup;

// 带有非平凡析构函数的 thread_local 对象由运行时延迟初始化。让分配辅助函数
// 持有该对象的引用，确保每个使用缓存的线程都注册清理回调。
inline void ensure_task_state_free_list_cleanup() noexcept
{
    (void)g_taskStateFreeListCleanup;
}

constexpr std::size_t kFrameSizeClasses[] = {
    128,
    256,
    512,
    1024,
    2048,
};
constexpr std::size_t kFrameSizeClassCount =
    sizeof(kFrameSizeClasses) / sizeof(kFrameSizeClasses[0]);
// 限制每个线程保留的 frame 缓存，避免突发清理后形成数 MiB 的常驻 RSS 底线。
constexpr std::size_t kFrameFreeListLimit = 256;
constexpr std::size_t kFrameDefaultAlignment = alignof(std::max_align_t);

struct FrameFreeNode
{
    FrameFreeNode* next = nullptr;
};

struct alignas(std::max_align_t) FrameAllocationHeader
{
    void* base = nullptr;
    std::uint64_t magic = 0;
    std::size_t alignment = 0;
    std::size_t bucket = kFrameSizeClassCount;
};

static_assert(sizeof(FrameAllocationHeader) % alignof(std::max_align_t) == 0);
constexpr std::uint64_t kFrameAllocationMagic = 0x47414c415946524dULL;

FrameAllocationHeader* frame_allocation_header(void* ptr) noexcept;

struct FrameFreeList
{
    FrameFreeNode* head = nullptr;
    std::size_t count = 0;

    ~FrameFreeList() noexcept
    {
        while (head != nullptr) {
            auto* node = head;
            head = node->next;
            if (auto* header = frame_allocation_header(node); header != nullptr) {
                ::operator delete(header->base,
                                  std::align_val_t(header->alignment));
            } else {
                ::operator delete(node);
            }
        }
        count = 0;
    }
};

thread_local FrameFreeList g_frameFreeListBuckets[kFrameSizeClassCount];
thread_local bool g_frameRecyclerEnabled = true;
thread_local bool g_failFrameAllocationForTesting = false;
thread_local bool g_failTaskStateAllocationForTesting = false;

std::size_t frame_size_class_index(std::size_t size) noexcept
{
    for (std::size_t index = 0; index < kFrameSizeClassCount; ++index) {
        if (size <= kFrameSizeClasses[index]) {
            return index;
        }
    }
    return kFrameSizeClassCount;
}

std::align_val_t frame_global_alignment(std::size_t alignment) noexcept
{
    if (alignment == 0 || alignment <= kFrameDefaultAlignment) {
        return std::align_val_t(kFrameDefaultAlignment);
    }
    return std::align_val_t(alignment);
}

std::uintptr_t align_address(std::uintptr_t address,
                            std::size_t alignment) noexcept
{
    const auto remainder = address % alignment;
    return remainder == 0 ? address : address + alignment - remainder;
}

void* allocate_frame_storage_block(std::size_t size,
                                std::size_t alignment,
                                std::size_t bucket) noexcept
{
    if (alignment == 0) {
        alignment = kFrameDefaultAlignment;
    }

    const auto max = std::numeric_limits<std::size_t>::max();
    // 在进入 sanitizer/libc 分配入口前拦截不可能的请求；部分分配器会在
    // nothrow 返回 nullptr 前直接诊断 SIZE_MAX 请求。
    const auto extra = alignment > kFrameDefaultAlignment
        ? alignment - 1
        : 0;
    if (extra > max - sizeof(FrameAllocationHeader) ||
        size > max - sizeof(FrameAllocationHeader) - extra) {
        return nullptr;
    }

    const auto total = size + sizeof(FrameAllocationHeader) + extra;
    const auto globalAlignment = frame_global_alignment(alignment);
    auto* base = ::operator new(total,
                                globalAlignment,
                                std::nothrow);
    if (base == nullptr) {
        return nullptr;
    }

    const auto first = reinterpret_cast<std::uintptr_t>(base) +
        sizeof(FrameAllocationHeader);
    const auto aligned = align_address(first, alignment);
    auto* header = reinterpret_cast<FrameAllocationHeader*>(
        aligned - sizeof(FrameAllocationHeader));
    header->base = base;
    header->magic = kFrameAllocationMagic;
    header->alignment = static_cast<std::size_t>(globalAlignment);
    header->bucket = bucket;
    return reinterpret_cast<void*>(aligned);
}

FrameAllocationHeader* frame_allocation_header(void* ptr) noexcept
{
    if (ptr == nullptr) {
        return nullptr;
    }

    auto* header = reinterpret_cast<FrameAllocationHeader*>(
        static_cast<std::byte*>(ptr) - sizeof(FrameAllocationHeader));
    return header->magic == kFrameAllocationMagic ? header : nullptr;
}

void release_frame_raw(void* ptr, std::size_t alignment) noexcept
{
    if (ptr == nullptr) {
        return;
    }

    if (auto* header = frame_allocation_header(ptr); header != nullptr) {
        ::operator delete(header->base, std::align_val_t(header->alignment));
        return;
    }
    if (alignment == 0 || alignment <= kFrameDefaultAlignment) {
        ::operator delete(ptr);
        return;
    }
    ::operator delete(ptr, frame_global_alignment(alignment));
}

void destroy_task_frame_for_state_teardown(TaskState* state) noexcept
{
    if (state == nullptr || state->m_handle == nullptr) {
        return;
    }

    // TaskState 只会在最后一个 owning TaskRef 释放后析构，此时不可能仍有协程执行。
    // 公共 helper 继续为显式调用保留更强的 done 状态保护；teardown 路径避免对已在
    // final_suspend() 自行销毁 frame 的完成任务执行多余的原子读取和外部调用。
    auto handle = state->m_handle;
    state->m_handle = nullptr;
    handle.destroy();
}

void* allocate_task_state_storage(std::size_t size, std::align_val_t alignment)
{
    ensure_task_state_free_list_cleanup();
    if (size == sizeof(TaskState) &&
        alignment == std::align_val_t(alignof(TaskState)) &&
        g_taskStateFreeList != nullptr) {
        auto* node = g_taskStateFreeList;
        g_taskStateFreeList = node->next;
        --g_taskStateFreeCount;
        return node;
    }
    return ::operator new(size, alignment);
}

void release_task_state_storage(void* ptr, std::size_t size, std::align_val_t alignment) noexcept
{
    if (ptr == nullptr) {
        return;
    }

    ensure_task_state_free_list_cleanup();

    if (size != sizeof(TaskState) ||
        alignment != std::align_val_t(alignof(TaskState)) ||
        g_taskStateFreeCount >= kTaskStateFreeListLimit) {
        ::operator delete(ptr, alignment);
        return;
    }

    auto* node = static_cast<TaskStateFreeNode*>(ptr);
    node->next = g_taskStateFreeList;
    g_taskStateFreeList = node;
    ++g_taskStateFreeCount;
}

} // namespace

namespace detail
{

void* allocate_frame_storage(std::size_t size, std::size_t alignment) noexcept
{
    if (alignment == 0) {
        alignment = kFrameDefaultAlignment;
    }
    if (g_failFrameAllocationForTesting) {
        return nullptr;
    }
    if (g_frameRecyclerEnabled && alignment <= kFrameDefaultAlignment) {
        const auto index = frame_size_class_index(size);
        if (index < kFrameSizeClassCount) {
            auto& bucket = g_frameFreeListBuckets[index];
            if (bucket.head != nullptr) {
                auto* node = bucket.head;
                bucket.head = node->next;
                --bucket.count;
                return node;
            }
            return allocate_frame_storage_block(kFrameSizeClasses[index], alignment, index);
        }
    }

    return allocate_frame_storage_block(size, alignment, kFrameSizeClassCount);
}

void release_frame_storage(void* ptr,
                         [[maybe_unused]] std::size_t size,
                         std::size_t alignment) noexcept
{
    if (ptr == nullptr) {
        return;
    }

    if (alignment == 0) {
        alignment = kFrameDefaultAlignment;
    }

    // 分配头记录实际的尺寸类别。不要根据编译器提供的 delete 尺寸推导缓存桶：
    // 调用方传入过期或不匹配的尺寸时，也不能将内存块放入更大的缓存桶。
    // size 参数仍属于编译器 delete ABI，但在此处有意忽略。
    auto* header = frame_allocation_header(ptr);
    if (header == nullptr ||
        header->bucket >= kFrameSizeClassCount ||
        alignment > kFrameDefaultAlignment || !g_frameRecyclerEnabled) {
        release_frame_raw(ptr, alignment);
        return;
    }

    auto& bucket = g_frameFreeListBuckets[header->bucket];
    if (bucket.count >= kFrameFreeListLimit) {
        release_frame_raw(ptr, alignment);
        return;
    }

    auto* node = static_cast<FrameFreeNode*>(ptr);
    node->next = bucket.head;
    bucket.head = node;
    ++bucket.count;
}

std::size_t frame_free_list_size_for_testing(std::size_t size,
                                        std::size_t alignment) noexcept
{
    if (alignment > kFrameDefaultAlignment) {
        return 0;
    }
    const auto index = frame_size_class_index(size);
    return index < kFrameSizeClassCount
        ? g_frameFreeListBuckets[index].count
        : 0;
}

void set_frame_recycler_enabled_for_testing(bool enabled) noexcept
{
    g_frameRecyclerEnabled = enabled;
}

void set_frame_allocation_failure_for_testing(bool enabled) noexcept
{
    g_failFrameAllocationForTesting = enabled;
}

void set_task_state_allocation_failure_for_testing(bool enabled) noexcept
{
    g_failTaskStateAllocationForTesting = enabled;
}

} // namespace detail

TaskState::~TaskState()
{
    // 已完成的 frame 会由 final_suspend() == suspend_never 销毁；只有未完成且
    // 尚未提交的 frame 才需要执行这条显式清理路径。
    destroy_task_frame_for_state_teardown(this);
    if (m_destroy_result != nullptr && m_result_kind != ResultStorageKind::Empty) {
        m_destroy_result(*this);
    }
}

void* TaskState::operator new(std::size_t size)
{
    return allocate_task_state_storage(size, std::align_val_t(alignof(TaskState)));
}

void* TaskState::operator new(std::size_t size, std::align_val_t alignment)
{
    return allocate_task_state_storage(size, alignment);
}

void* TaskState::operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    ensure_task_state_free_list_cleanup();
    if (g_failTaskStateAllocationForTesting) {
        return nullptr;
    }
    if (size == sizeof(TaskState) && g_taskStateFreeList != nullptr) {
        auto* node = g_taskStateFreeList;
        g_taskStateFreeList = node->next;
        --g_taskStateFreeCount;
        return node;
    }
    return ::operator new(size,
                          std::align_val_t(alignof(TaskState)),
                          std::nothrow);
}

void* TaskState::operator new(std::size_t size,
                              std::align_val_t alignment,
                              const std::nothrow_t&) noexcept
{
    ensure_task_state_free_list_cleanup();
    if (g_failTaskStateAllocationForTesting) {
        return nullptr;
    }
    if (size == sizeof(TaskState) &&
        alignment == std::align_val_t(alignof(TaskState)) &&
        g_taskStateFreeList != nullptr) {
        auto* node = g_taskStateFreeList;
        g_taskStateFreeList = node->next;
        --g_taskStateFreeCount;
        return node;
    }
    return ::operator new(size, alignment, std::nothrow);
}

void TaskState::operator delete(void* ptr) noexcept
{
    release_task_state_storage(ptr, sizeof(TaskState), std::align_val_t(alignof(TaskState)));
}

void TaskState::operator delete(void* ptr, std::size_t size) noexcept
{
    release_task_state_storage(ptr, size, std::align_val_t(alignof(TaskState)));
}

void TaskState::operator delete(void* ptr, std::align_val_t alignment) noexcept
{
    release_task_state_storage(ptr, sizeof(TaskState), alignment);
}

void TaskState::operator delete(void* ptr, std::size_t size, std::align_val_t alignment) noexcept
{
    release_task_state_storage(ptr, size, alignment);
}

namespace detail
{

bool destroy_task_frame(TaskState* state) noexcept
{
    if (state == nullptr || state->is_done() ||
        state->m_handle == nullptr) {
        return false;
    }

    auto handle = state->m_handle;
    state->m_handle = nullptr;
    handle.destroy();
    return true;
}

} // namespace detail

namespace detail
{

Runtime* current_runtime() noexcept
{
    return g_currentRuntime;
}

Runtime* swap_current_runtime(Runtime* runtime) noexcept
{
    Runtime* previous = g_currentRuntime;
    g_currentRuntime = runtime;
    return previous;
}

bool schedule_task(const TaskRef& task) noexcept
{
    auto* scheduler = task.belong_scheduler();
    return scheduler != nullptr && scheduler->schedule(task);
}

bool schedule_task_deferred(const TaskRef& task) noexcept
{
    auto* scheduler = task.belong_scheduler();
    return scheduler != nullptr && scheduler->schedule_deferred(task);
}

bool schedule_task_deferred_state(TaskState* state) noexcept
{
    if (state == nullptr || state->m_scheduler == nullptr) {
        return false;
    }
    return state->m_scheduler->schedule_deferred(TaskRef(state, true));
}

bool schedule_task_immediately(const TaskRef& task) noexcept
{
    auto* scheduler = task.belong_scheduler();
    return scheduler != nullptr && scheduler->schedule_immediately(task);
}

bool request_task_resume(const TaskRef& task) noexcept
{
    return request_task_resume_state_detailed(task.state()) ==
        TaskResumeResult::kAccepted;
}

TaskResumeResult request_task_resume_state_detailed(TaskState* state) noexcept
{
    if (!state || !state->m_handle || !state->m_scheduler ||
        state->is_done(std::memory_order_relaxed)) {
        return TaskResumeResult::kRejected;
    }

    if (state->m_queued.exchange(true, std::memory_order_acq_rel)) {
        return TaskResumeResult::kAlreadyQueued;
    }
    state->m_resume_owner_only.store(true, std::memory_order_release);
    if (state->m_scheduler->schedule_resume(TaskRef(state, true))) {
        return TaskResumeResult::kAccepted;
    }

    state->m_resume_owner_only.store(false, std::memory_order_release);
    state->m_queued.store(false, std::memory_order_release);
    return TaskResumeResult::kRejected;
}

bool request_task_resume_state(TaskState* state) noexcept
{
    return request_task_resume_state_detailed(state) == TaskResumeResult::kAccepted;
}

std::thread::id scheduler_thread_id(Scheduler* scheduler) noexcept
{
    return scheduler ? scheduler->thread_id() : std::thread::id{};
}

void attach_task_continuation(const TaskRef& task, TaskRef next) noexcept
{
    auto* state = task.state();
    if (state == nullptr) {
        return;
    }

    inherit_task_runtime(next, state->m_runtime);
    if (next.belong_scheduler() == nullptr && state->m_scheduler != nullptr) {
        set_task_scheduler(next, state->m_scheduler);
    }
    state->m_then = std::move(next);
}

void complete_task_state(const TaskRef& task) noexcept
{
    complete_task_state(task.state());
}

void complete_task_state(TaskState* state) noexcept
{
    if (!state) {
        return;
    }

    // exchange 与 kPending -> kWaiting 注册在同一原子修改序中：注册先发生就
    // 通知全部等待者；完成先发生则注册失败并 acquire 观察结果，无需 seq_cst。
    // 无阻塞等待者时不调用 notify_all，避免进入标准库共享等待池。
    if (state->m_completion_status.exchange(TaskCompletionStatus::kDone,
                                            std::memory_order_release) ==
        TaskCompletionStatus::kWaiting) {
        state->m_completion_status.notify_all();
    }

    auto schedule_continuation = [](std::optional<TaskRef>& continuation) {
        if (!continuation.has_value()) {
            return;
        }

        TaskRef next = std::move(*continuation);
        continuation.reset();
        if (request_task_resume(next)) {
            return;
        }

        auto* nextState = next.state();
        auto* scheduler = next.belong_scheduler();
        bool expected = false;
        // stop() 会先关闭 resume admission 再排空普通任务。只有 owner 线程
        // 可以把 completion continuation 降级到普通延后队列，避免跨线程恢复。
        if (nextState != nullptr && scheduler != nullptr &&
            !nextState->is_done() &&
            std::this_thread::get_id() == scheduler_thread_id(scheduler) &&
            nextState->m_queued.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            nextState->m_resume_owner_only.store(true,
                                                 std::memory_order_release);
            if (scheduler->schedule_deferred(next)) {
                return;
            }
            nextState->m_resume_owner_only.store(false,
                                                 std::memory_order_release);
            nextState->m_queued.store(false, std::memory_order_release);
        }

        if (nextState != nullptr &&
            !nextState->is_done() &&
            !nextState->m_queued.load(std::memory_order_acquire)) {
            continuation = std::move(next);
        }
    };

    schedule_continuation(state->m_then);
    schedule_continuation(state->m_next);
}

bool wait_task_completion(const TaskRef& task)
{
    auto* state = task.state();
    if (state == nullptr) {
        return false;
    }

    auto status = state->m_completion_status.load(std::memory_order_acquire);
    if (status == TaskCompletionStatus::kPending) {
        if (state->m_completion_status.compare_exchange_strong(
                status,
                TaskCompletionStatus::kWaiting,
                std::memory_order_acquire,
                std::memory_order_acquire)) {
            status = TaskCompletionStatus::kWaiting;
        }
    }
    if (status == TaskCompletionStatus::kWaiting) {
        // wait 在值已变为 kDone 时直接返回；即使通知先于入睡也不会丢唤醒。
        // kDone 是终态，不会出现 ABA；acquire 与完成方的 release 发布结果。
        state->m_completion_status.wait(TaskCompletionStatus::kWaiting,
                                         std::memory_order_acquire);
    }
    return true;
}

} // namespace detail

} // namespace galay::kernel
