/**
 * @file task.cc
 * @brief 任务状态分配器、TaskRef 生命周期及任务调度辅助函数
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 实现：
 * - TaskState 对象的线程局部空闲链分配器
 * - TaskRef 引用计数（retain/release）
 * - TaskState 析构函数和等待器清理
 * - 任务生命周期辅助函数：调度、完成、等待、continuation 附加
 * - 线程局部 Runtime 作用域管理（g_currentRuntime）
 */

#include "task.h"
#include "scheduler.hpp"

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
constexpr size_t kTaskStateFreeListLimit = 1024;

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
thread_local bool g_failTaskStateAllocationForTesting = false;

constexpr std::size_t kFrameSizeClasses[] = {
    128,
    256,
    512,
    1024,
    2048,
};
constexpr std::size_t kFrameSizeClassCount =
    sizeof(kFrameSizeClasses) / sizeof(kFrameSizeClasses[0]);
constexpr std::size_t kFrameFreeListLimit = 1024;
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
    std::size_t bucket = 0;
};

static_assert(sizeof(FrameAllocationHeader) % alignof(std::max_align_t) == 0);
constexpr std::uint64_t kFrameAllocationMagic = 0x47414c415946524dULL;

struct FrameFreeList
{
    FrameFreeNode* head = nullptr;
    std::size_t count = 0;

    ~FrameFreeList() noexcept
    {
        while (head != nullptr) {
            auto* node = head;
            head = node->next;
            auto* header = reinterpret_cast<FrameAllocationHeader*>(
                static_cast<std::byte*>(static_cast<void*>(node)) -
                sizeof(FrameAllocationHeader));
            ::operator delete(header->base,
                              std::align_val_t(header->alignment));
        }
        count = 0;
    }
};

thread_local FrameFreeList g_frameFreeListBuckets[kFrameSizeClassCount];
thread_local bool g_frameRecyclerEnabled = true;
thread_local bool g_failFrameAllocationForTesting = false;

std::size_t frameSizeClassIndex(std::size_t size) noexcept
{
    for (std::size_t index = 0; index < kFrameSizeClassCount; ++index) {
        if (size <= kFrameSizeClasses[index]) {
            return index;
        }
    }
    return kFrameSizeClassCount;
}

std::align_val_t frameGlobalAlignment(std::size_t alignment) noexcept
{
    if (alignment == 0 || alignment <= kFrameDefaultAlignment) {
        return std::align_val_t(kFrameDefaultAlignment);
    }
    return std::align_val_t(alignment);
}

std::uintptr_t alignAddress(std::uintptr_t address,
                            std::size_t alignment) noexcept
{
    const auto remainder = address % alignment;
    return remainder == 0 ? address : address + alignment - remainder;
}

void* allocateFrameStorageBlock(std::size_t size,
                                std::size_t alignment,
                                std::size_t bucket) noexcept
{
    if (alignment == 0) {
        alignment = kFrameDefaultAlignment;
    }
    const auto extra = alignment > kFrameDefaultAlignment
        ? alignment - 1
        : 0;
    const auto max = std::numeric_limits<std::size_t>::max();
    if (extra > max - sizeof(FrameAllocationHeader) ||
        size > max - sizeof(FrameAllocationHeader) - extra) {
        return nullptr;
    }

    const auto total = size + sizeof(FrameAllocationHeader) + extra;
    auto* base = ::operator new(total,
                                frameGlobalAlignment(alignment),
                                std::nothrow);
    if (base == nullptr) {
        return nullptr;
    }

    const auto first = reinterpret_cast<std::uintptr_t>(base) +
        sizeof(FrameAllocationHeader);
    const auto aligned = alignAddress(first, alignment);
    auto* header = reinterpret_cast<FrameAllocationHeader*>(
        aligned - sizeof(FrameAllocationHeader));
    header->base = base;
    header->magic = kFrameAllocationMagic;
    header->alignment = static_cast<std::size_t>(frameGlobalAlignment(alignment));
    header->bucket = bucket;
    return reinterpret_cast<void*>(aligned);
}

FrameAllocationHeader* frameAllocationHeader(void* ptr) noexcept
{
    if (ptr == nullptr) {
        return nullptr;
    }

    auto* header = reinterpret_cast<FrameAllocationHeader*>(
        static_cast<std::byte*>(ptr) - sizeof(FrameAllocationHeader));
    return header->magic == kFrameAllocationMagic ? header : nullptr;
}

void releaseFrameRaw(void* ptr, std::size_t alignment) noexcept
{
    if (ptr == nullptr) {
        return;
    }
    if (auto* header = frameAllocationHeader(ptr); header != nullptr) {
        ::operator delete(header->base, std::align_val_t(header->alignment));
        return;
    }
    ::operator delete(ptr, frameGlobalAlignment(alignment));
}

void* allocateTaskStateStorage(std::size_t size, std::align_val_t alignment)
{
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

void releaseTaskStateStorage(void* ptr, std::size_t size, std::align_val_t alignment) noexcept
{
    if (ptr == nullptr) {
        return;
    }

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

void* allocateFrameStorage(std::size_t size, std::size_t alignment) noexcept
{
    if (alignment == 0) {
        alignment = kFrameDefaultAlignment;
    }
    if (g_failFrameAllocationForTesting) {
        return nullptr;
    }
    if (g_frameRecyclerEnabled && alignment <= kFrameDefaultAlignment) {
        const auto index = frameSizeClassIndex(size);
        if (index < kFrameSizeClassCount) {
            auto& bucket = g_frameFreeListBuckets[index];
            if (bucket.head != nullptr) {
                auto* node = bucket.head;
                bucket.head = node->next;
                --bucket.count;
                return node;
            }
            return allocateFrameStorageBlock(kFrameSizeClasses[index],
                                              alignment,
                                              index);
        }
    }

    return allocateFrameStorageBlock(size, alignment, kFrameSizeClassCount);
}

void releaseFrameStorage(void* ptr,
                         std::size_t size,
                         std::size_t alignment) noexcept
{
    if (ptr == nullptr) {
        return;
    }

    // The header, rather than a guessed size class, is authoritative for all
    // frame blocks returned by this allocator, including unsized delete.
    (void)size;
    auto* header = frameAllocationHeader(ptr);
    if (header == nullptr) {
        releaseFrameRaw(ptr, alignment);
        return;
    }

    const auto index = g_frameRecyclerEnabled
        ? header->bucket
        : kFrameSizeClassCount;
    if (index >= kFrameSizeClassCount) {
        releaseFrameRaw(ptr, alignment);
        return;
    }

    auto& bucket = g_frameFreeListBuckets[index];
    if (bucket.count >= kFrameFreeListLimit) {
        releaseFrameRaw(ptr, alignment);
        return;
    }

    auto* node = static_cast<FrameFreeNode*>(ptr);
    node->next = bucket.head;
    bucket.head = node;
    ++bucket.count;
}

std::size_t frameFreeListSizeForTesting(std::size_t size,
                                        std::size_t alignment) noexcept
{
    if (alignment > kFrameDefaultAlignment) {
        return 0;
    }
    const auto index = frameSizeClassIndex(size);
    return index < kFrameSizeClassCount
        ? g_frameFreeListBuckets[index].count
        : 0;
}

void setFrameRecyclerEnabledForTesting(bool enabled) noexcept
{
    g_frameRecyclerEnabled = enabled;
}

void setFrameAllocationFailureForTesting(bool enabled) noexcept
{
    g_failFrameAllocationForTesting = enabled;
}

void setTaskStateAllocationFailureForTesting(bool enabled) noexcept
{
    g_failTaskStateAllocationForTesting = enabled;
}

} // namespace detail

TaskState::~TaskState()
{
    // Completed frames are destroyed by final_suspend() == suspend_never;
    // only an incomplete, unsubmitted frame can need this explicit cleanup.
    (void)detail::destroyTaskFrame(this);
    if (m_destroy_result != nullptr && m_result_kind != ResultStorageKind::Empty) {
        m_destroy_result(*this);
    }

    TaskWaiter* waiter = m_waiter.load(std::memory_order_acquire);
    delete waiter;
}

void* TaskState::operator new(std::size_t size)
{
    return allocateTaskStateStorage(size, std::align_val_t(alignof(TaskState)));
}

void* TaskState::operator new(std::size_t size, std::align_val_t alignment)
{
    return allocateTaskStateStorage(size, alignment);
}

void* TaskState::operator new(std::size_t size, const std::nothrow_t&) noexcept
{
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
    releaseTaskStateStorage(ptr, sizeof(TaskState), std::align_val_t(alignof(TaskState)));
}

void TaskState::operator delete(void* ptr, std::size_t size) noexcept
{
    releaseTaskStateStorage(ptr, size, std::align_val_t(alignof(TaskState)));
}

void TaskState::operator delete(void* ptr, std::align_val_t alignment) noexcept
{
    releaseTaskStateStorage(ptr, sizeof(TaskState), alignment);
}

void TaskState::operator delete(void* ptr, std::size_t size, std::align_val_t alignment) noexcept
{
    releaseTaskStateStorage(ptr, size, alignment);
}

namespace detail
{

bool destroyTaskFrame(TaskState* state) noexcept
{
    if (state == nullptr || state->m_done.load(std::memory_order_acquire) ||
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

namespace
{

TaskWaiter& ensureTaskWaiter(TaskState& state)
{
    TaskWaiter* waiter = state.m_waiter.load(std::memory_order_acquire);
    if (waiter != nullptr) {
        return *waiter;
    }

    auto* candidate = new TaskWaiter();
    if (state.m_waiter.compare_exchange_strong(waiter,
                                               candidate,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
        return *candidate;
    }

    delete candidate;
    return *waiter;
}

void notifyTaskWaiters(TaskState& state)
{
    TaskWaiter* waiter = state.m_waiter.load(std::memory_order_acquire);
    if (waiter == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(waiter->m_mutex);
    waiter->m_cv.notify_all();
}

} // namespace

Runtime* currentRuntime() noexcept
{
    return g_currentRuntime;
}

Runtime* swapCurrentRuntime(Runtime* runtime) noexcept
{
    Runtime* previous = g_currentRuntime;
    g_currentRuntime = runtime;
    return previous;
}

bool scheduleTask(const TaskRef& task) noexcept
{
    auto* scheduler = task.belongScheduler();
    return scheduler != nullptr && scheduler->schedule(task);
}

bool scheduleTaskDeferred(const TaskRef& task) noexcept
{
    auto* scheduler = task.belongScheduler();
    return scheduler != nullptr && scheduler->scheduleDeferred(task);
}

bool scheduleTaskImmediately(const TaskRef& task) noexcept
{
    auto* scheduler = task.belongScheduler();
    return scheduler != nullptr && scheduler->scheduleImmediately(task);
}

bool requestTaskResume(const TaskRef& task) noexcept
{
    return requestTaskResumeState(task.state());
}

bool requestTaskResumeState(TaskState* state) noexcept
{
    if (!state || !state->m_handle || !state->m_scheduler ||
        state->m_done.load(std::memory_order_relaxed)) {
        return false;
    }

    if (state->m_queued.exchange(true, std::memory_order_acq_rel)) {
        return false;
    }
    state->m_resume_owner_only.store(true, std::memory_order_release);
    if (state->m_scheduler->scheduleResume(TaskRef(state, true))) {
        return true;
    }

    state->m_resume_owner_only.store(false, std::memory_order_release);
    state->m_queued.store(false, std::memory_order_release);
    return false;
}

std::thread::id schedulerThreadId(Scheduler* scheduler) noexcept
{
    return scheduler ? scheduler->threadId() : std::thread::id{};
}

void attachTaskContinuation(const TaskRef& task, TaskRef next) noexcept
{
    auto* state = task.state();
    if (state == nullptr) {
        return;
    }

    inheritTaskRuntime(next, state->m_runtime);
    if (next.belongScheduler() == nullptr && state->m_scheduler != nullptr) {
        setTaskScheduler(next, state->m_scheduler);
    }
    state->m_then = std::move(next);
}

void completeTaskState(const TaskRef& task) noexcept
{
    auto* state = task.state();
    if (!state) {
        return;
    }

    state->m_done.store(true, std::memory_order_release);
    notifyTaskWaiters(*state);

    auto schedule_continuation = [](std::optional<TaskRef>& continuation) {
        if (!continuation.has_value()) {
            return;
        }

        TaskRef next = std::move(*continuation);
        continuation.reset();
        if (requestTaskResume(next)) {
            return;
        }

        auto* nextState = next.state();
        auto* scheduler = next.belongScheduler();
        bool expected = false;
        // stop() 会先关闭 resume admission 再排空普通任务。只有 owner 线程
        // 可以把 completion continuation 降级到普通延后队列，避免跨线程恢复。
        if (nextState != nullptr && scheduler != nullptr &&
            !nextState->m_done.load(std::memory_order_acquire) &&
            std::this_thread::get_id() == schedulerThreadId(scheduler) &&
            nextState->m_queued.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            nextState->m_resume_owner_only.store(true,
                                                 std::memory_order_release);
            if (scheduler->scheduleDeferred(next)) {
                return;
            }
            nextState->m_resume_owner_only.store(false,
                                                 std::memory_order_release);
            nextState->m_queued.store(false, std::memory_order_release);
        }

        if (nextState != nullptr &&
            !nextState->m_done.load(std::memory_order_acquire) &&
            !nextState->m_queued.load(std::memory_order_acquire)) {
            continuation = std::move(next);
        }
    };

    schedule_continuation(state->m_then);
    schedule_continuation(state->m_next);
}

bool waitTaskCompletion(const TaskRef& task)
{
    auto* state = task.state();
    if (state == nullptr) {
        return false;
    }

    while (!state->m_done.load(std::memory_order_acquire)) {
        TaskWaiter& waiter = ensureTaskWaiter(*state);
        std::unique_lock<std::mutex> lock(waiter.m_mutex);
        if (state->m_done.load(std::memory_order_acquire)) {
            return true;
        }
        waiter.m_cv.wait(lock, [state]() {
            return state->m_done.load(std::memory_order_acquire);
        });
    }
    return true;
}

} // namespace detail

TaskRef::TaskRef(TaskState* state, bool retainRef) noexcept
    : m_state(state)
{
    if (retainRef) {
        retain();
    }
}

TaskRef TaskRef::borrowed(TaskState* state) noexcept
{
    TaskRef result;
    if (state == nullptr) {
        return result;
    }

    const auto raw = reinterpret_cast<uintptr_t>(state);
    result.m_state = reinterpret_cast<TaskState*>(raw | kBorrowedBit);
    return result;
}

TaskRef::TaskRef(const TaskRef& other) noexcept
    : m_state(other.state())
{
    retain();
}

TaskRef::TaskRef(TaskRef&& other) noexcept
    : m_state(other.m_state)
{
    other.m_state = nullptr;
}

TaskRef::~TaskRef()
{
    release();
}

TaskRef& TaskRef::operator=(const TaskRef& other) noexcept
{
    if (this != &other) {
        auto* state = other.state();
        if (state != nullptr) {
            // Retain before releasing the old value so assigning from a
            // borrowed view of the same state cannot invalidate the source.
            state->m_refs.fetch_add(1, std::memory_order_relaxed);
        }
        release();
        m_state = state;
    }
    return *this;
}

TaskRef& TaskRef::operator=(TaskRef&& other) noexcept
{
    if (this != &other) {
        if (other.isBorrowed() && state() == other.state()) {
            // A borrowed promise view carries no reference to transfer. Keep
            // the existing owner and only invalidate the moved-from view.
            other.m_state = nullptr;
            return *this;
        }
        release();
        m_state = other.m_state;
        other.m_state = nullptr;
    }
    return *this;
}

Scheduler* TaskRef::belongScheduler() const noexcept
{
    auto* state = this->state();
    return state ? state->m_scheduler : nullptr;
}

void TaskRef::retain() noexcept
{
    if (auto* state = this->state()) {
        state->m_refs.fetch_add(1, std::memory_order_relaxed);
    }
}

void TaskRef::release() noexcept
{
    if (!m_state) {
        return;
    }

    if (isBorrowed()) {
        m_state = nullptr;
        return;
    }

    auto* state = this->state();
    m_state = nullptr;
    if (state->m_refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        delete state;
    }
}

} // namespace galay::kernel
