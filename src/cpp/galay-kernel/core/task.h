/**
 * @file task.h
 * @brief 协程任务、promise 和 join handle 基础组件
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 定义 Task<T>、JoinHandle<T>、TaskPromise<T>、TaskAwaiter<T> 和
 *          阻塞任务完成态。TaskState/TaskRef 位于 task_ref.h；调度器 ready
 *          queue 类型位于 ready_entry.h，普通协程使用方无需承担该依赖。
 */

#ifndef GALAY_KERNEL_TASK_H
#define GALAY_KERNEL_TASK_H

#include "task_ref.h"

#include <atomic>
#include <coroutine>
#include <cstddef>
#include <expected>
#include <memory>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

namespace galay::kernel
{

class Runtime;
template <typename T>
class TaskPromise;
template <typename T>
class Task;
template <typename T>
class JoinHandle;

namespace detail
{

struct TaskAccess;
template <typename T>
class TaskAwaiter;
template <typename T, typename Promise>
class TaskPromiseBase;

template <typename T>
struct TaskResultStorageTraits
{
    static_assert(!std::is_reference_v<T>, "Task<T> does not support reference results");

    static constexpr bool kInline =
        sizeof(T) <= TaskState::kInlineResultBytes &&
        alignof(T) <= alignof(std::max_align_t);

    static void destroy(TaskState& state) noexcept
    {
        if constexpr (kInline) {
            if (state.m_result_kind == TaskState::ResultStorageKind::Inline) {
                std::destroy_at(reinterpret_cast<T*>(state.result_storage()));
            }
        } else {
            if (state.m_result_kind == TaskState::ResultStorageKind::Heap) {
                auto*& ptr = *reinterpret_cast<T**>(state.result_storage());
                delete ptr;
                ptr = nullptr;
            }
        }
        state.m_result_kind = TaskState::ResultStorageKind::Empty;
    }

    template <typename U>
    static void store(TaskState& state, U&& value)
    {
        if constexpr (kInline) {
            std::construct_at(reinterpret_cast<T*>(state.result_storage()), std::forward<U>(value));
            state.m_result_kind = TaskState::ResultStorageKind::Inline;
        } else {
            *reinterpret_cast<T**>(state.result_storage()) = new T(std::forward<U>(value));
            state.m_result_kind = TaskState::ResultStorageKind::Heap;
        }
    }

    static std::expected<T, TaskResultError> try_take(TaskState& state)
    {
        if constexpr (kInline) {
            if (state.m_result_kind != TaskState::ResultStorageKind::Inline) {
                return std::unexpected(TaskResultError(TaskResultErrorCode::kResultUnavailable));
            }
            T value = std::move(*reinterpret_cast<T*>(state.result_storage()));
            destroy(state);
            return value;
        }

        if (state.m_result_kind != TaskState::ResultStorageKind::Heap) {
            return std::unexpected(TaskResultError(TaskResultErrorCode::kResultUnavailable));
        }
        auto*& ptr = *reinterpret_cast<T**>(state.result_storage());
        auto* result = ptr;
        ptr = nullptr;
        state.m_result_kind = TaskState::ResultStorageKind::Empty;
        std::unique_ptr<T> holder(result);
        return std::move(*holder);
    }
};

template <typename T>
void initialize_task_result(TaskState* state) noexcept
{
    if (state != nullptr) {
        state->m_destroy_result = &TaskResultStorageTraits<T>::destroy;
    }
}

template <>
inline void initialize_task_result<void>(TaskState* state) noexcept
{
    if (state != nullptr) {
        state->m_destroy_result = nullptr;
    }
}

template <typename T, typename U>
bool store_task_result(TaskState* state, U&& value)
{
    if (state == nullptr) {
        return false;
    }
    TaskResultStorageTraits<T>::store(*state, std::forward<U>(value));
    return true;
}

inline void store_task_error(const TaskRef& task, TaskResultError error) noexcept
{
    if (auto* state = task.state()) {
        state->m_result_error = std::move(error);
    }
}

inline void store_task_error(TaskState* state, TaskResultError error) noexcept
{
    if (state != nullptr) {
        state->m_result_error = std::move(error);
    }
}

template <typename T>
std::expected<T, TaskResultError> try_take_task_result(const TaskRef& task)
{
    auto* state = task.state();
    if (state == nullptr) {
        return std::unexpected(TaskResultError(TaskResultErrorCode::kInvalidState));
    }

    if (!wait_task_completion(task)) {
        return std::unexpected(TaskResultError(TaskResultErrorCode::kInvalidState));
    }
    if (state->m_result_error.has_value()) {
        return std::unexpected(*state->m_result_error);
    }
    if (state->m_result_consumed.exchange(true, std::memory_order_acq_rel)) {
        return std::unexpected(TaskResultError(TaskResultErrorCode::kAlreadyConsumed));
    }

    if constexpr (std::is_void_v<T>) {
        return {};
    } else {
        return TaskResultStorageTraits<T>::try_take(*state);
    }
}

} // namespace detail

/**
 * @brief 任务完成态存储
 * @tparam T 任务结果类型
 * @details 为 `JoinHandle<T>` 和 `TaskAwaiter<T>` 提供线程安全的结果/异常交付。
 */
template <typename T>
struct TaskCompletionState
{
    static_assert(!std::is_reference_v<T>, "Task<T> does not support reference results");

    /**
     * @brief 写入任务返回值并唤醒等待者
     * @tparam U 可转换到 `T` 的值类型
     * @param value 要保存的任务结果
     * @return 无返回值
     */
    template <typename U>
    void set_value(U&& value)
    {
        m_value = std::forward<U>(value);
        m_completion_status.store(detail::TaskCompletionStatus::kDone,
                                  std::memory_order_release);
        m_completion_status.notify_all();
    }

    /**
     * @brief 写入任务错误并唤醒等待者
     * @param error 要返回给 join() 的任务错误
     * @return 无返回值
     */
    void set_error(detail::TaskResultError error)
    {
        m_error = std::move(error);
        m_completion_status.store(detail::TaskCompletionStatus::kDone,
                                  std::memory_order_release);
        m_completion_status.notify_all();
    }

    /**
     * @brief 阻塞等待任务结束
     * @return 无返回值
     * @note 只等待完成，不消耗结果
     */
    void wait() const
    {
        wait_ready();
    }

    /**
     * @brief 取走任务结果
     * @return 成功时返回任务结果；重复消费或结果缺失时返回 TaskResultError
     */
    std::expected<T, detail::TaskResultError> take()
    {
        wait_ready();
        if (m_consumed.exchange(true, std::memory_order_acq_rel)) {
            return std::unexpected(detail::TaskResultError(detail::TaskResultErrorCode::kAlreadyConsumed));
        }
        if (m_error.has_value()) {
            return std::unexpected(*m_error);
        }
        if (!m_value.has_value()) {
            return std::unexpected(detail::TaskResultError(detail::TaskResultErrorCode::kResultUnavailable));
        }
        return std::move(*m_value);
    }

private:
    /** @brief 等待生产者发布完成状态；acquire 保证结果字段可见。 */
    void wait_ready() const
    {
        auto status = m_completion_status.load(std::memory_order_acquire);
        while (status != detail::TaskCompletionStatus::kDone) {
            m_completion_status.wait(status, std::memory_order_acquire);
            status = m_completion_status.load(std::memory_order_acquire);
        }
    }

    std::optional<T> m_value;
    std::optional<detail::TaskResultError> m_error;
    std::atomic<detail::TaskCompletionStatus> m_completion_status{
        detail::TaskCompletionStatus::kPending};  ///< 完成状态与等待通知共享同一原子地址
    std::atomic<bool> m_consumed{false};  ///< 保证并发 take() 只有一个消费者取得结果
};

/**
 * @brief `void` 任务完成态存储特化
 * @details 只传递完成/异常信号，不保存值。
 */
template <>
struct TaskCompletionState<void>
{
    void set_value()  ///< 标记任务成功完成并唤醒等待者
    {
        m_completion_status.store(detail::TaskCompletionStatus::kDone,
                                  std::memory_order_release);
        m_completion_status.notify_all();
    }

    void set_error(detail::TaskResultError error)  ///< 写入任务错误并唤醒等待者
    {
        m_error = std::move(error);
        m_completion_status.store(detail::TaskCompletionStatus::kDone,
                                  std::memory_order_release);
        m_completion_status.notify_all();
    }

    void wait() const  ///< 阻塞等待任务结束，不消耗完成状态
    {
        wait_ready();
    }

    std::expected<void, detail::TaskResultError> take()  ///< 消费完成状态；重复消费时返回错误
    {
        wait_ready();
        if (m_consumed.exchange(true, std::memory_order_acq_rel)) {
            return std::unexpected(detail::TaskResultError(detail::TaskResultErrorCode::kAlreadyConsumed));
        }
        if (m_error.has_value()) {
            return std::unexpected(*m_error);
        }
        return {};
    }

private:
    /** @brief 等待生产者发布完成状态；acquire 保证错误字段可见。 */
    void wait_ready() const
    {
        auto status = m_completion_status.load(std::memory_order_acquire);
        while (status != detail::TaskCompletionStatus::kDone) {
            m_completion_status.wait(status, std::memory_order_acquire);
            status = m_completion_status.load(std::memory_order_acquire);
        }
    }

    std::optional<detail::TaskResultError> m_error;
    std::atomic<detail::TaskCompletionStatus> m_completion_status{
        detail::TaskCompletionStatus::kPending};  ///< 完成状态与等待通知共享同一原子地址
    std::atomic<bool> m_consumed{false};  ///< 保证并发 take() 只有一个消费者取得结果
};

/**
 * @brief 可移动的协程任务拥有者
 * @tparam T 协程结果类型
 * @details `Task<T>` 持有底层协程及其完成态，支持 `co_await` 或交给 Runtime 调度。
 */
template <typename T>
class Task
{
public:
    using promise_type = TaskPromise<T>;  ///< 与该任务类型配套的 coroutine promise

    Task() noexcept = default;  ///< 构造空任务
    /**
     * @brief 移动任务所有权
     * @param other 源对象
     */
    Task(Task&& other) noexcept = default;
    /**
     * @brief 移动赋值任务所有权
     * @param other 源对象
     * @return 当前对象引用
     */
    Task& operator=(Task&& other) noexcept = default;

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    /**
     * @brief 是否持有可用任务
     * @return 满足所检查条件时返回 true，否则返回 false
     */
    bool is_valid() const { return m_task.is_valid(); }
    bool done() const
    {
        auto* state = m_task.state();
        return !state || state->is_done();
    }

    /**
     * @brief 以左值任务创建 awaiter；恢复后会消费任务结果
     * @return 操作结果，类型由函数实现推导
     */
    auto operator co_await() &;
    /**
     * @brief 以右值任务创建 awaiter；恢复后会消费任务结果
     * @return 操作结果，类型由函数实现推导
     */
    auto operator co_await() &&;
    /**
     * @brief 为 `void` 任务追加 continuation，返回当前左值引用
     * @param next 后继对象
     * @return Task<T>& 引用
     */
    Task& then(Task next) &
        requires std::is_void_v<T>;
    /**
     * @brief 为 `void` 任务追加 continuation，返回当前右值引用
     * @param next 后继对象
     * @return Task<T>&& 引用
     */
    Task&& then(Task next) &&
        requires std::is_void_v<T>;

private:
    friend class Runtime;
    template <typename U>
    friend class JoinHandle;
    template <typename U>
    friend class TaskPromise;
    template <typename U, typename Promise>
    friend class detail::TaskPromiseBase;
    friend struct detail::TaskAccess;

    explicit Task(TaskRef task) noexcept
        : m_task(std::move(task))
    {
    }

    std::expected<T, detail::TaskResultError> take_result()  ///< 取走任务结果；失败时返回 TaskResultError
    {
        return detail::try_take_task_result<T>(m_task);
    }

    TaskRef m_task;
};

/**
 * @brief 同步等待任务结果的句柄
 * @tparam T 任务结果类型
 * @details 可跨线程使用；`join()` 会消费结果，`wait()` 只等待完成。
 */
template <typename T>
class JoinHandle
{
public:
    JoinHandle() noexcept = default;  ///< 构造空句柄
    explicit JoinHandle(TaskRef task) noexcept
        : m_task(std::move(task))
    {
    }
    explicit JoinHandle(std::shared_ptr<TaskCompletionState<T>> completion) noexcept
        : m_blocking_completion(std::move(completion))
    {
    }
    /**
     * @brief 移动句柄所有权
     * @param other 源对象
     */
    JoinHandle(JoinHandle&& other) noexcept = default;
    /**
     * @brief 移动赋值句柄所有权
     * @param other 源对象
     * @return 当前对象引用
     */
    JoinHandle& operator=(JoinHandle&& other) noexcept = default;

    JoinHandle(const JoinHandle&) = delete;
    JoinHandle& operator=(const JoinHandle&) = delete;

    /**
     * @brief 是否绑定到有效任务完成态
     * @return 满足所检查条件时返回 true，否则返回 false
     */
    bool is_valid() const noexcept { return m_task.is_valid() || static_cast<bool>(m_blocking_completion); }

    std::expected<void, detail::TaskResultError> wait() const  ///< 阻塞等待任务结束，不消费结果
    {
        if (m_task.is_valid()) {
            if (!detail::wait_task_completion(m_task)) {
                return std::unexpected(detail::TaskResultError(detail::TaskResultErrorCode::kInvalidState));
            }
            return {};
        }
        if (m_blocking_completion) {
            m_blocking_completion->wait();
            return {};
        }
        return std::unexpected(detail::TaskResultError(detail::TaskResultErrorCode::kInvalidState));
    }

    std::expected<T, detail::TaskResultError> join()  ///< 阻塞等待并消费结果
    {
        if (m_task.is_valid()) {
            return detail::try_take_task_result<T>(m_task);
        }
        if (m_blocking_completion) {
            return m_blocking_completion->take();
        }
        return std::unexpected(detail::TaskResultError(detail::TaskResultErrorCode::kInvalidState));
    }

private:
    TaskRef m_task;
    std::shared_ptr<TaskCompletionState<T>> m_blocking_completion;
};

namespace detail
{

/**
 * @brief Task 私有状态访问器
 * @details 供 Runtime、Scheduler 和 awaiter 实现读取或拆解 Task。
 */
struct TaskAccess
{
    template <typename T>
    static const TaskRef& task_ref(const Task<T>& task) noexcept  ///< 返回任务引用视图，不转移所有权
    {
        return task.m_task;
    }

    template <typename T>
    static decltype(auto) take_result(Task<T>& task)  ///< 消费并返回任务结果
    {
        return task.take_result();
    }

    template <typename T>
    static std::expected<T, TaskResultError> try_take_result(Task<T>& task)  ///< 以返回值消费任务结果
    {
        return try_take_task_result<T>(task.m_task);
    }

    template <typename T>
    static TaskRef detach_task(Task<T>&& task) noexcept  ///< 从 Task 中拆出底层任务引用并转移所有权
    {
        return std::move(task.m_task);
    }
};

} // namespace detail

namespace detail
{

/**
 * @brief `co_await Task<T>` 使用的 awaiter
 * @tparam T 任务结果类型
 * @details 在父任务挂起前安排子任务执行，并在子任务完成后恢复父任务。
 */
template <typename T>
class TaskAwaiter
{
public:
    explicit TaskAwaiter(Task<T>&& task) noexcept  ///< 接管待等待的子任务
        : m_task(std::move(task))
    {
    }

    bool await_ready() const noexcept  ///< 子任务已完成或无效时无需挂起父任务
    {
        return !m_task.is_valid() || m_task.done();
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle)  ///< 安排子任务执行，并在必要时挂起父任务
    {
        TaskRef waitingTask = handle.promise().task_ref_view();
        // awaiter 已持有子任务；调度器入队时取得自己的引用，无需临时增加引用计数。
        const TaskRef& childTask = TaskAccess::task_ref(m_task);
        if (!childTask.is_valid()) {
            return false;
        }
        if (m_task.done()) {
            return false;
        }

        detail::inherit_task_runtime(childTask, detail::task_runtime(waitingTask));
        auto* scheduler = waitingTask.belong_scheduler();
        if (scheduler == nullptr) {
            detail::store_task_error(childTask, TaskResultError(TaskResultErrorCode::kScheduleFailed));
            detail::complete_task_state(childTask);
            return false;
        }
        if (childTask.belong_scheduler() == nullptr) {
            detail::set_task_scheduler(childTask, scheduler);
        }
        childTask.state()->m_next = std::move(waitingTask);
        if (!detail::schedule_task_immediately(childTask)) {
            childTask.state()->m_next = TaskRef{};
            detail::store_task_error(childTask, TaskResultError(TaskResultErrorCode::kScheduleFailed));
            detail::complete_task_state(childTask);
            return false;
        }
        return true;
    }

    std::expected<T, TaskResultError> await_resume()  ///< 返回或消费子任务结果
    {
        return TaskAccess::take_result(m_task);
    }

private:
    Task<T> m_task;
};

} // namespace detail

template <typename T>
inline auto Task<T>::operator co_await() &  ///< 以左值任务创建 awaiter
{
    return detail::TaskAwaiter<T>(std::move(*this));
}

template <typename T>
inline auto Task<T>::operator co_await() &&  ///< 以右值任务创建 awaiter
{
    return detail::TaskAwaiter<T>(std::move(*this));
}

template <typename T>
inline Task<T>& Task<T>::then(Task<T> next) &
    requires std::is_void_v<T>  ///< 为 void 任务追加 continuation，返回左值引用
{
    detail::attach_task_continuation(m_task, detail::TaskAccess::detach_task(std::move(next)));
    return *this;
}

template <typename T>
inline Task<T>&& Task<T>::then(Task<T> next) &&
    requires std::is_void_v<T>  ///< 为 void 任务追加 continuation，返回右值引用
{
    detail::attach_task_continuation(m_task, detail::TaskAccess::detach_task(std::move(next)));
    return std::move(*this);
}

namespace detail
{

template <typename T, typename Promise>
class TaskPromiseBase
{
public:
    using ReSchedulerType = bool;  ///< `co_yield true/false` 使用的重新调度标记类型

    static void* operator new(std::size_t size) noexcept
    {
        return detail::allocate_frame_storage(size, alignof(std::max_align_t));
    }

    static void* operator new(std::size_t size,
                              std::align_val_t alignment) noexcept
    {
        return detail::allocate_frame_storage(size,
                                            static_cast<std::size_t>(alignment));
    }

    static void operator delete(void* ptr) noexcept
    {
        detail::release_frame_storage(ptr, 0, alignof(std::max_align_t));
    }

    static void operator delete(void* ptr, std::size_t size) noexcept
    {
        detail::release_frame_storage(ptr, size, alignof(std::max_align_t));
    }

    static void operator delete(void* ptr, std::align_val_t alignment) noexcept
    {
        detail::release_frame_storage(ptr, 0, static_cast<std::size_t>(alignment));
    }

    static void operator delete(void* ptr,
                                std::size_t size,
                                std::align_val_t alignment) noexcept
    {
        detail::release_frame_storage(ptr,
                                     size,
                                     static_cast<std::size_t>(alignment));
    }

    static void* operator new(std::size_t size, const std::nothrow_t&) noexcept
    {
        return detail::allocate_frame_storage(size, alignof(std::max_align_t));
    }

    static void* operator new(std::size_t size,
                              std::align_val_t alignment,
                              const std::nothrow_t&) noexcept
    {
        return detail::allocate_frame_storage(size,
                                            static_cast<std::size_t>(alignment));
    }

    /**
     * @brief 协程帧分配失败时返回无效任务
     * @return 无有效协程帧的 Task，is_valid() 为 false；消费结果时返回 kInvalidState 错误
     */
    static Task<T> get_return_object_on_allocation_failure() noexcept
    {
        return {};
    }

    /** @brief 返回当前协程共享状态；仅供 CRTP 派生 promise 的返回入口使用。 */
    TaskState* task_state() const noexcept { return m_state; }

    Task<T> get_return_object() noexcept  ///< 构造并返回与该 promise 绑定的 Task
    {
        auto* promise = static_cast<Promise*>(this);
        auto handle = std::coroutine_handle<Promise>::from_promise(*promise);
        auto* state = new (std::nothrow) TaskState(handle);
        if (state == nullptr) {
            return {};
        }
        m_state = state;
        m_task_view = TaskRefStorageAccess::borrow(state);
        detail::initialize_task_result<T>(state);
        detail::inherit_task_runtime(state, detail::current_runtime());
        return Task<T>(TaskRef(state, false));
    }

    struct InitialSuspendAwaiter
    {
        Promise* promise = nullptr;

        bool await_ready() const noexcept { return false; }

        bool await_suspend(std::coroutine_handle<> handle) const noexcept
        {
            if (promise->m_state == nullptr) {
                // frame 创建后 TaskState 分配失败。此分支中的 frame 尚未持有状态，
                // 因此趁它仍处于挂起状态时在此销毁，并返回无效 Task。
                handle.destroy();
            }
            return true;
        }

        void await_resume() const noexcept {}
    };

    /**
     * @brief 初始总是挂起，失败状态在挂起点释放 frame
     * @return InitialSuspendAwaiter 操作结果
     */
    InitialSuspendAwaiter initial_suspend() noexcept
    {
        return {static_cast<Promise*>(this)};
    }

    std::suspend_always yield_value(ReSchedulerType flag) noexcept  ///< `co_yield true` 时把任务重新放回延后队列
    {
        if (flag) {
            detail::schedule_task_deferred_state(m_state);
        }
        return {};
    }

    /**
     * @brief 结束时不再二次挂起，由完成逻辑直接清理
     * @return std::suspend_never 操作结果
     */
    std::suspend_never final_suspend() noexcept { return {}; }

    void unhandled_exception() noexcept  ///< 捕获协程异常并写入完成态
    {
        detail::store_task_error(m_state, detail::TaskResultError(detail::TaskResultErrorCode::kTaskException));
        detail::complete_task_state(m_state);
    }

    ~TaskPromiseBase() noexcept
    {
        if (m_state != nullptr) {
            // 编译器和显式调用 coroutine_handle::destroy() 都可能在不进入
            // TaskState::~TaskState() 的情况下释放 frame。清除非拥有的 handle view，
            // 避免后续状态清理再次尝试销毁 frame。
            m_state->m_handle = nullptr;
        }
        m_state = nullptr;
        m_task_view = TaskRef{};
    }

    /**
     * @brief 返回非拥有 TaskRef view
     * @return const TaskRef& 引用
     */
    const TaskRef& task_ref_view() const noexcept { return m_task_view; }

private:
    TaskState* m_state = nullptr;  ///< promise 热路径使用的非拥有状态指针
    mutable TaskRef m_task_view;  ///< 为兼容 awaiter 保留的非拥有 TaskRef view
};

} // namespace detail

/**
 * @brief 有返回值协程的 promise
 * @tparam T 任务结果类型
 * @details 生命周期、分配和调度骨架由 `TaskPromiseBase<T, TaskPromise<T>>` 提供，
 *          特化只负责写入协程返回值。
 */
template <typename T>
class TaskPromise : public detail::TaskPromiseBase<T, TaskPromise<T>>
{
public:
    template <typename U>
    void return_value(U&& value) noexcept(std::is_nothrow_constructible_v<T, U&&>)  ///< 写入协程返回值并标记完成
    {
        auto* state = this->task_state();
        if (!detail::store_task_result<T>(state, std::forward<U>(value))) {
            detail::store_task_error(state,
                                     detail::TaskResultError(detail::TaskResultErrorCode::kInvalidState));
        }
        detail::complete_task_state(state);
    }
};

/**
 * @brief `void` 协程的 promise 特化
 * @details 与有返回值 promise 共用生命周期和调度骨架，只实现无返回值完成入口。
 */
template <>
class TaskPromise<void> : public detail::TaskPromiseBase<void, TaskPromise<void>>
{
public:
    void return_void() noexcept  ///< 标记 `void` 协程成功完成
    {
        detail::complete_task_state(this->task_state());
    }
};

} // namespace galay::kernel

#endif // GALAY_KERNEL_TASK_H
