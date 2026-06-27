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

void* allocateTaskStateStorage(std::size_t size, std::align_val_t alignment)
{
    if (size == sizeof(TaskState) && g_taskStateFreeList != nullptr) {
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

    if (size != sizeof(TaskState) || g_taskStateFreeCount >= kTaskStateFreeListLimit) {
        ::operator delete(ptr, alignment);
        return;
    }

    auto* node = static_cast<TaskStateFreeNode*>(ptr);
    node->next = g_taskStateFreeList;
    g_taskStateFreeList = node;
    ++g_taskStateFreeCount;
}

} // namespace

TaskState::~TaskState()
{
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
    if (state->m_scheduler->schedule(TaskRef(state, true))) {
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

    if (state->m_then.has_value()) {
        TaskRef nextThen = std::move(*state->m_then);
        state->m_then.reset();
        if (auto* scheduler = nextThen.belongScheduler()) {
            scheduler->schedule(std::move(nextThen));
        }
    }

    if (state->m_next.has_value()) {
        TaskRef next = std::move(*state->m_next);
        state->m_next.reset();
        if (auto* scheduler = next.belongScheduler()) {
            scheduler->schedule(std::move(next));
        }
    }
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

TaskRef::TaskRef(const TaskRef& other) noexcept
    : m_state(other.m_state)
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
        release();
        m_state = other.m_state;
        retain();
    }
    return *this;
}

TaskRef& TaskRef::operator=(TaskRef&& other) noexcept
{
    if (this != &other) {
        release();
        m_state = other.m_state;
        other.m_state = nullptr;
    }
    return *this;
}

Scheduler* TaskRef::belongScheduler() const noexcept
{
    return m_state ? m_state->m_scheduler : nullptr;
}

void TaskRef::retain() noexcept
{
    if (m_state) {
        m_state->m_refs.fetch_add(1, std::memory_order_relaxed);
    }
}

void TaskRef::release() noexcept
{
    if (!m_state) {
        return;
    }

    auto* state = m_state;
    m_state = nullptr;
    if (state->m_refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        delete state;
    }
}

} // namespace galay::kernel
