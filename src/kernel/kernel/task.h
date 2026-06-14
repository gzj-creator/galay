/**
 * @file task.h
 * @brief 协程任务、promise 和 join handle 基础组件
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 定义核心协程基础设施：
 * - TaskState：共享状态块，持有 handle、调度器、运行时和结果存储
 * - TaskRef：TaskState 的轻量引用计数句柄
 * - Task<T>：只移动的协程所有者，支持 co_await
 * - JoinHandle<T>：同步结果消费者，支持 wait/join 语义
 * - TaskPromise<T>：协程 promise，负责创建 Task 和处理完成逻辑
 * - TaskCompletionState<T>：用于阻塞 spawn 的线程安全结果/异常交付
 * - TaskAwaiter<T>：链接父子协程恢复的 awaiter
 *
 * 同时提供 TaskState 的线程局部空闲链分配器，以减少热路径上的分配开销。
 */

#ifndef GALAY_KERNEL_TASK_H
#define GALAY_KERNEL_TASK_H

#include <atomic>
#include <array>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace galay::kernel
{

class Scheduler;  ///< 调度器前置声明
class Runtime;  ///< Runtime 前置声明
template <typename T>
class TaskPromise;  ///< 协程 promise 前置声明
template <typename T>
class Task;  ///< 协程任务句柄前置声明
template <typename T>
class JoinHandle;  ///< 同步等待任务结果的句柄前置声明
struct TaskState;  ///< 任务共享状态前置声明
class TaskRef;  ///< 轻量任务引用前置声明
struct TaskWaiter;  ///< 任务等待器前置声明

namespace detail
{

struct TaskRefStorageAccess;  ///< 供固定容量调度 ring 在 TaskRef 与裸状态指针间转移所有权

/**
 * @brief 内部任务结果消费错误类别。
 */
enum class TaskResultErrorCode : uint8_t {
    kInvalidState,       ///< TaskRef 没有关联有效 TaskState
    kTaskException,      ///< 协程执行过程中记录了异常
    kAlreadyConsumed,    ///< 任务结果已被 join/await/blockOn 消费
    kResultUnavailable,  ///< 任务完成但结果存储不可用
    kScheduleFailed      ///< 子任务无法提交到 scheduler
};

/**
 * @brief 内部任务结果消费错误。
 * @details 只在 kernel 实现层传播；public Runtime API 会映射为 RuntimeError。
 */
class TaskResultError
{
public:
    explicit TaskResultError(TaskResultErrorCode error_code) noexcept
        : m_code(error_code)
    {
    }

    TaskResultErrorCode code() const noexcept { return m_code; }  ///< 返回任务结果消费错误类别
    std::string_view message() const noexcept
    {
        static constexpr std::array<std::string_view, static_cast<size_t>(TaskResultErrorCode::kScheduleFailed) + 1> kMessages = {{
            "task state is invalid",
            "task completed with an unhandled exception",
            "task result has already been consumed",
            "task completed without an available result",
            "task could not be scheduled for execution"
        }};

        const auto index = static_cast<size_t>(m_code);
        if (index < kMessages.size()) {
            return kMessages[index];
        }
        return "unknown task result error";
    }

private:
    TaskResultErrorCode m_code;
};

Runtime* currentRuntime() noexcept;  ///< 读取当前线程绑定的 Runtime，上下文不存在时返回 nullptr
Runtime* swapCurrentRuntime(Runtime* runtime) noexcept;  ///< 替换当前线程 Runtime 并返回旧值
bool scheduleTask(const TaskRef& task) noexcept;  ///< 将任务按普通语义提交给其所属调度器
bool scheduleTaskDeferred(const TaskRef& task) noexcept;  ///< 将任务按延后语义提交给其所属调度器
bool scheduleTaskImmediately(const TaskRef& task) noexcept;  ///< 在所属调度器线程上立即恢复任务
bool requestTaskResume(const TaskRef& task) noexcept;  ///< 请求恢复已暂停任务；失败时返回 false
std::thread::id schedulerThreadId(Scheduler* scheduler) noexcept;  ///< 查询调度器线程 ID；scheduler 为空时返回默认值
void completeTaskState(const TaskRef& task) noexcept;  ///< 标记任务完成并触发 continuation 清理
void attachTaskContinuation(const TaskRef& task, TaskRef next) noexcept;  ///< 为任务追加下一段 continuation
bool waitTaskCompletion(const TaskRef& task);  ///< 阻塞等待任务完成；无有效任务状态时返回 false
void storeTaskError(const TaskRef& task, TaskResultError error) noexcept;  ///< 写入任务错误
struct TaskAccess;  ///< 供内核实现访问 Task 私有状态的辅助入口
template <typename T>
class TaskAwaiter;  ///< `co_await Task<T>` 使用的 awaiter
template <typename T>
void initializeTaskResult(const TaskRef& task) noexcept;  ///< 初始化任务结果存储
template <typename T, typename U>
bool storeTaskResult(const TaskRef& task, U&& value);  ///< 写入任务结果
template <typename T>
std::expected<T, TaskResultError> tryTakeTaskResult(const TaskRef& task);  ///< 以返回值消费任务结果

} // namespace detail

/**
 * @brief 任务状态的轻量引用句柄
 * @details 采用引用计数管理底层 `TaskState` 生命周期，可跨调度器和 continuation 链传递。
 */
class TaskRef
{
public:
    TaskRef() noexcept = default;
    explicit TaskRef(TaskState* state, bool retainRef) noexcept;  ///< 从裸状态创建引用；`retainRef=true` 时增加引用计数
    TaskRef(const TaskRef& other) noexcept;  ///< 拷贝并共享同一底层状态
    TaskRef(TaskRef&& other) noexcept;  ///< 移动任务引用，源对象被清空
    ~TaskRef();  ///< 释放引用；最后一个引用会回收底层状态

    TaskRef& operator=(const TaskRef& other) noexcept;  ///< 拷贝赋值并共享同一底层状态
    TaskRef& operator=(TaskRef&& other) noexcept;  ///< 移动赋值，源对象被清空

    bool isValid() const noexcept { return m_state != nullptr; }  ///< 是否引用到有效任务状态
    TaskState* state() const noexcept { return m_state; }  ///< 返回底层任务状态裸指针，不转移所有权
    Scheduler* belongScheduler() const noexcept;  ///< 返回任务所属调度器；未绑定时返回 nullptr

private:
    template <typename T>
    friend class Task;
    template <typename T>
    friend class TaskPromise;
    friend struct detail::TaskRefStorageAccess;

    void retain() noexcept;  ///< 增加底层状态引用计数
    void release() noexcept;  ///< 减少底层状态引用计数，必要时释放状态

    TaskState* m_state = nullptr;
};

/**
 * @brief 协程任务的共享状态块
 * @details 保存 coroutine handle、所属调度器、Runtime 上下文和 continuation 链。
 */
struct alignas(64) TaskState
{
    static constexpr size_t kInlineResultBytes = 32;
    enum class ResultStorageKind : uint8_t { Empty, Inline, Heap };

    template <typename Promise>
    explicit TaskState(std::coroutine_handle<Promise> handle) noexcept
        : m_handle(handle) {}

    ~TaskState();

    static void* operator new(std::size_t size);
    static void* operator new(std::size_t size, std::align_val_t alignment);
    static void operator delete(void* ptr) noexcept;
    static void operator delete(void* ptr, std::size_t size) noexcept;
    static void operator delete(void* ptr, std::align_val_t alignment) noexcept;
    static void operator delete(void* ptr, std::size_t size, std::align_val_t alignment) noexcept;

    void* resultStorage() noexcept { return static_cast<void*>(m_result_storage); }
    const void* resultStorage() const noexcept { return static_cast<const void*>(m_result_storage); }

    std::coroutine_handle<> m_handle = nullptr;  ///< 底层协程句柄
    Scheduler* m_scheduler = nullptr;  ///< 任务所属调度器
    Runtime* m_runtime = nullptr;  ///< 任务继承到的 Runtime 上下文
    std::optional<TaskRef> m_then;  ///< `then()` 追加的 continuation 任务
    std::optional<TaskRef> m_next;  ///< 当前 `co_await` 恢复后要继续唤醒的父任务
    void (*m_destroy_result)(TaskState&) noexcept = nullptr;  ///< 销毁尚未消费的结果对象
    std::optional<detail::TaskResultError> m_result_error;  ///< 任务错误
    std::atomic<TaskWaiter*> m_waiter{nullptr};  ///< 惰性分配的等待器，仅 join/wait 路径需要
    std::atomic<uint32_t> m_refs{1};  ///< TaskRef 引用计数
    std::atomic<bool> m_done{false};  ///< 任务是否已经执行完成
    std::atomic<bool> m_queued{false};  ///< 任务是否已在调度队列中
    std::atomic<bool> m_resume_owner_only{false};  ///< 本次由 waker/timeout 恢复的任务必须回到 owner scheduler 线程执行
    std::atomic<bool> m_result_consumed{false};  ///< 任务结果是否已被 join/await 消费
    ResultStorageKind m_result_kind = ResultStorageKind::Empty;  ///< 当前结果的存储形态
    alignas(std::max_align_t) std::byte m_result_storage[kInlineResultBytes]{};  ///< 小对象内联结果存储
};

struct TaskWaiter
{
    std::mutex m_mutex;
    std::condition_variable m_cv;
};

namespace detail
{

inline Runtime* taskRuntime(const TaskRef& task) noexcept
{
    auto* state = task.state();
    return state ? state->m_runtime : nullptr;
}

inline void setTaskRuntime(const TaskRef& task, Runtime* runtime) noexcept
{
    if (auto* state = task.state()) {
        state->m_runtime = runtime;
        if (state->m_then.has_value()) {
            if (auto* thenState = state->m_then->state(); thenState && thenState->m_runtime == nullptr) {
                thenState->m_runtime = runtime;
            }
        }
    }
}

inline void inheritTaskRuntime(const TaskRef& task, Runtime* runtime) noexcept
{
    if (auto* state = task.state(); state && state->m_runtime == nullptr) {
        state->m_runtime = runtime;
    }
}

inline void setTaskScheduler(const TaskRef& task, Scheduler* scheduler) noexcept
{
    if (auto* state = task.state()) {
        state->m_scheduler = scheduler;
        if (state->m_then.has_value() && state->m_then->belongScheduler() == nullptr) {
            setTaskScheduler(*state->m_then, scheduler);
        }
    }
}

/**
 * @brief 作用域化切换当前线程 Runtime
 * @details 构造时替换线程局部 Runtime，析构时自动恢复旧值。
 */
class CurrentRuntimeScope
{
public:
    explicit CurrentRuntimeScope(Runtime* runtime) noexcept
        : m_previous(swapCurrentRuntime(runtime)) {}

    ~CurrentRuntimeScope()
    {
        swapCurrentRuntime(m_previous);
    }

    CurrentRuntimeScope(const CurrentRuntimeScope&) = delete;
    CurrentRuntimeScope& operator=(const CurrentRuntimeScope&) = delete;

private:
    Runtime* m_previous;
};

} // namespace detail

namespace detail
{

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
                std::destroy_at(reinterpret_cast<T*>(state.resultStorage()));
            }
        } else {
            if (state.m_result_kind == TaskState::ResultStorageKind::Heap) {
                auto*& ptr = *reinterpret_cast<T**>(state.resultStorage());
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
            std::construct_at(reinterpret_cast<T*>(state.resultStorage()), std::forward<U>(value));
            state.m_result_kind = TaskState::ResultStorageKind::Inline;
        } else {
            *reinterpret_cast<T**>(state.resultStorage()) = new T(std::forward<U>(value));
            state.m_result_kind = TaskState::ResultStorageKind::Heap;
        }
    }

    static std::expected<T, TaskResultError> tryTake(TaskState& state)
    {
        if constexpr (kInline) {
            if (state.m_result_kind != TaskState::ResultStorageKind::Inline) {
                return std::unexpected(TaskResultError(TaskResultErrorCode::kResultUnavailable));
            }
            T value = std::move(*reinterpret_cast<T*>(state.resultStorage()));
            destroy(state);
            return value;
        }

        if (state.m_result_kind != TaskState::ResultStorageKind::Heap) {
            return std::unexpected(TaskResultError(TaskResultErrorCode::kResultUnavailable));
        }
        auto*& ptr = *reinterpret_cast<T**>(state.resultStorage());
        auto* result = ptr;
        ptr = nullptr;
        state.m_result_kind = TaskState::ResultStorageKind::Empty;
        std::unique_ptr<T> holder(result);
        return std::move(*holder);
    }
};

template <typename T>
void initializeTaskResult(const TaskRef& task) noexcept
{
    if (auto* state = task.state()) {
        state->m_destroy_result = &TaskResultStorageTraits<T>::destroy;
    }
}

template <>
inline void initializeTaskResult<void>(const TaskRef& task) noexcept
{
    if (auto* state = task.state()) {
        state->m_destroy_result = nullptr;
    }
}

template <typename T, typename U>
bool storeTaskResult(const TaskRef& task, U&& value)
{
    auto* state = task.state();
    if (state == nullptr) {
        return false;
    }
    TaskResultStorageTraits<T>::store(*state, std::forward<U>(value));
    return true;
}

inline void storeTaskError(const TaskRef& task, TaskResultError error) noexcept
{
    if (auto* state = task.state()) {
        state->m_result_error = std::move(error);
    }
}

template <typename T>
std::expected<T, TaskResultError> tryTakeTaskResult(const TaskRef& task)
{
    auto* state = task.state();
    if (state == nullptr) {
        return std::unexpected(TaskResultError(TaskResultErrorCode::kInvalidState));
    }

    if (!waitTaskCompletion(task)) {
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
        return TaskResultStorageTraits<T>::tryTake(*state);
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
     */
    template <typename U>
    void setValue(U&& value)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_value = std::forward<U>(value);
            m_ready = true;
        }
        m_cv.notify_all();
    }

    /**
     * @brief 阻塞等待任务结束
     * @note 只等待完成，不消耗结果
     */
    void wait() const
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this]() { return m_ready; });
    }

    /**
     * @brief 取走任务结果
     * @return 成功时返回任务结果；重复消费或结果缺失时返回 TaskResultError
     */
    std::expected<T, detail::TaskResultError> take()
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this]() { return m_ready; });
        if (m_consumed) {
            return std::unexpected(detail::TaskResultError(detail::TaskResultErrorCode::kAlreadyConsumed));
        }
        if (!m_value.has_value()) {
            return std::unexpected(detail::TaskResultError(detail::TaskResultErrorCode::kResultUnavailable));
        }
        m_consumed = true;
        return std::move(*m_value);
    }

private:
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_cv;
    std::optional<T> m_value;
    bool m_ready = false;
    bool m_consumed = false;
};

/**
 * @brief `void` 任务完成态存储特化
 * @details 只传递完成/异常信号，不保存值。
 */
template <>
struct TaskCompletionState<void>
{
    void setValue()  ///< 标记任务成功完成并唤醒等待者
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_ready = true;
        }
        m_cv.notify_all();
    }

    void wait() const  ///< 阻塞等待任务结束，不消耗完成状态
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this]() { return m_ready; });
    }

    std::expected<void, detail::TaskResultError> take()  ///< 消费完成状态；重复消费时返回错误
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this]() { return m_ready; });
        if (m_consumed) {
            return std::unexpected(detail::TaskResultError(detail::TaskResultErrorCode::kAlreadyConsumed));
        }
        m_consumed = true;
        return {};
    }

private:
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_cv;
    bool m_ready = false;
    bool m_consumed = false;
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
    Task(Task&& other) noexcept = default;  ///< 移动任务所有权
    Task& operator=(Task&& other) noexcept = default;  ///< 移动赋值任务所有权

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    bool isValid() const { return m_task.isValid(); }  ///< 是否持有可用任务
    bool done() const
    {
        auto* state = m_task.state();
        return !state || state->m_done.load(std::memory_order_acquire);
    }

    auto operator co_await() &;  ///< 以左值任务创建 awaiter；恢复后会消费任务结果
    auto operator co_await() &&;  ///< 以右值任务创建 awaiter；恢复后会消费任务结果

private:
    friend class Runtime;
    template <typename U>
    friend class JoinHandle;
    template <typename U>
    friend class TaskPromise;
    friend struct detail::TaskAccess;

    explicit Task(TaskRef task) noexcept
        : m_task(std::move(task))
    {
    }

    std::expected<T, detail::TaskResultError> takeResult()  ///< 取走任务结果；失败时返回 TaskResultError
    {
        return detail::tryTakeTaskResult<T>(m_task);
    }

    TaskRef m_task;
};

/**
 * @brief `void` 任务特化
 * @details 除无返回值外，其生命周期和调度语义与 `Task<T>` 一致。
 */
template <>
class Task<void>
{
public:
    using promise_type = TaskPromise<void>;  ///< 与该任务类型配套的 coroutine promise

    Task() noexcept = default;  ///< 构造空任务
    Task(Task&& other) noexcept = default;  ///< 移动任务所有权
    Task& operator=(Task&& other) noexcept = default;  ///< 移动赋值任务所有权

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    bool isValid() const { return m_task.isValid(); }  ///< 是否持有可用任务
    bool done() const
    {
        auto* state = m_task.state();
        return !state || state->m_done.load(std::memory_order_acquire);
    }

    auto operator co_await() &;  ///< 以左值任务创建 awaiter；恢复后只消费完成状态
    auto operator co_await() &&;  ///< 以右值任务创建 awaiter；恢复后只消费完成状态
    Task<void>& then(Task<void> next) &;  ///< 为当前任务追加 continuation，返回当前左值引用
    Task<void>&& then(Task<void> next) &&;  ///< 为当前任务追加 continuation，返回当前右值引用

private:
    friend class Runtime;
    template <typename U>
    friend class JoinHandle;
    friend class TaskPromise<void>;
    friend struct detail::TaskAccess;

    explicit Task(TaskRef task) noexcept
        : m_task(std::move(task))
    {
    }

    std::expected<void, detail::TaskResultError> takeResult()  ///< 消费完成状态；失败时返回 TaskResultError
    {
        return detail::tryTakeTaskResult<void>(m_task);
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
    JoinHandle(JoinHandle&& other) noexcept = default;  ///< 移动句柄所有权
    JoinHandle& operator=(JoinHandle&& other) noexcept = default;  ///< 移动赋值句柄所有权

    JoinHandle(const JoinHandle&) = delete;
    JoinHandle& operator=(const JoinHandle&) = delete;

    bool isValid() const noexcept { return m_task.isValid() || static_cast<bool>(m_blocking_completion); }  ///< 是否绑定到有效任务完成态

    std::expected<void, detail::TaskResultError> wait() const  ///< 阻塞等待任务结束，不消费结果
    {
        if (m_task.isValid()) {
            if (!detail::waitTaskCompletion(m_task)) {
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
        if (m_task.isValid()) {
            return detail::tryTakeTaskResult<T>(m_task);
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

/**
 * @brief `void` 任务的 join handle 特化
 */
template <>
class JoinHandle<void>
{
public:
    JoinHandle() noexcept = default;  ///< 构造空句柄
    explicit JoinHandle(TaskRef task) noexcept
        : m_task(std::move(task))
    {
    }
    explicit JoinHandle(std::shared_ptr<TaskCompletionState<void>> completion) noexcept
        : m_blocking_completion(std::move(completion))
    {
    }
    JoinHandle(JoinHandle&& other) noexcept = default;  ///< 移动句柄所有权
    JoinHandle& operator=(JoinHandle&& other) noexcept = default;  ///< 移动赋值句柄所有权

    JoinHandle(const JoinHandle&) = delete;
    JoinHandle& operator=(const JoinHandle&) = delete;

    bool isValid() const noexcept { return m_task.isValid() || static_cast<bool>(m_blocking_completion); }  ///< 是否绑定到有效任务完成态

    std::expected<void, detail::TaskResultError> wait() const  ///< 阻塞等待任务结束，不消费完成状态
    {
        if (m_task.isValid()) {
            if (!detail::waitTaskCompletion(m_task)) {
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

    std::expected<void, detail::TaskResultError> join()  ///< 阻塞等待并消费完成状态
    {
        if (m_task.isValid()) {
            return detail::tryTakeTaskResult<void>(m_task);
        }
        if (m_blocking_completion) {
            return m_blocking_completion->take();
        }
        return std::unexpected(detail::TaskResultError(detail::TaskResultErrorCode::kInvalidState));
    }

private:
    TaskRef m_task;
    std::shared_ptr<TaskCompletionState<void>> m_blocking_completion;
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
    static const TaskRef& taskRef(const Task<T>& task) noexcept  ///< 返回任务引用视图，不转移所有权
    {
        return task.m_task;
    }

    template <typename T>
    static decltype(auto) takeResult(Task<T>& task)  ///< 消费并返回任务结果
    {
        return task.takeResult();
    }

    template <typename T>
    static std::expected<T, TaskResultError> tryTakeResult(Task<T>& task)  ///< 以返回值消费任务结果
    {
        return tryTakeTaskResult<T>(task.m_task);
    }

    template <typename T>
    static TaskRef detachTask(Task<T>&& task) noexcept  ///< 从 Task 中拆出底层任务引用并转移所有权
    {
        return std::move(task.m_task);
    }
};

struct TaskRefStorageAccess
{
    static TaskState* releaseState(TaskRef& task) noexcept
    {
        TaskState* state = task.m_state;
        task.m_state = nullptr;
        return state;
    }

    static TaskRef adoptState(TaskState* state) noexcept
    {
        return TaskRef(state, false);
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
        return !m_task.isValid() || m_task.done();
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle)  ///< 安排子任务执行，并在必要时挂起父任务
    {
        TaskRef waitingTask = handle.promise().taskRefView();
        TaskRef childTask = TaskAccess::taskRef(m_task);
        if (!childTask.isValid()) {
            return false;
        }
        if (m_task.done()) {
            return false;
        }

        detail::inheritTaskRuntime(childTask, detail::taskRuntime(waitingTask));
        auto* scheduler = waitingTask.belongScheduler();
        if (scheduler == nullptr) {
            detail::storeTaskError(childTask, TaskResultError(TaskResultErrorCode::kScheduleFailed));
            detail::completeTaskState(childTask);
            return false;
        }
        if (childTask.belongScheduler() == nullptr) {
            detail::setTaskScheduler(childTask, scheduler);
        }
        if (!detail::scheduleTaskImmediately(childTask)) {
            detail::storeTaskError(childTask, TaskResultError(TaskResultErrorCode::kScheduleFailed));
            detail::completeTaskState(childTask);
            return false;
        }
        if (m_task.done()) {
            return false;
        }
        childTask.state()->m_next = std::move(waitingTask);
        return true;
    }

    std::expected<T, TaskResultError> await_resume()  ///< 返回或消费子任务结果
    {
        return TaskAccess::takeResult(m_task);
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

inline auto Task<void>::operator co_await() &  ///< 以左值 void 任务创建 awaiter
{
    return detail::TaskAwaiter<void>(std::move(*this));
}

inline auto Task<void>::operator co_await() &&  ///< 以右值 void 任务创建 awaiter
{
    return detail::TaskAwaiter<void>(std::move(*this));
}

inline Task<void>& Task<void>::then(Task<void> next) &  ///< 为当前任务追加 continuation，返回左值引用
{
    detail::attachTaskContinuation(m_task, detail::TaskAccess::detachTask(std::move(next)));
    return *this;
}

inline Task<void>&& Task<void>::then(Task<void> next) &&  ///< 为当前任务追加 continuation，返回右值引用
{
    detail::attachTaskContinuation(m_task, detail::TaskAccess::detachTask(std::move(next)));
    return std::move(*this);
}

/**
 * @brief 协程 promise
 * @tparam T 任务结果类型
 * @details 负责构造 `Task<T>`、记录完成结果，并在最终挂起前通知调度系统。
 */
template <typename T>
class TaskPromise
{
public:
    using ReSchedulerType = bool;  ///< `co_yield true/false` 使用的重新调度标记类型

    int get_return_object_on_alloaction_failure() noexcept { return -1; }  ///< 协程分配失败时返回错误码占位

    Task<T> get_return_object() noexcept  ///< 构造并返回与该 promise 绑定的 Task
    {
        auto handle = std::coroutine_handle<TaskPromise<T>>::from_promise(*this);
        m_task = TaskRef(new TaskState(handle), false);
        detail::initializeTaskResult<T>(m_task);
        detail::inheritTaskRuntime(m_task, detail::currentRuntime());
        return Task<T>(m_task);
    }

    std::suspend_always initial_suspend() noexcept { return {}; }  ///< 初始总是挂起，交由调度器决定首次恢复时机

    std::suspend_always yield_value(ReSchedulerType flag) noexcept  ///< `co_yield true` 时把任务重新放回延后队列
    {
        if (flag) {
            detail::scheduleTaskDeferred(m_task);
        }
        return {};
    }

    std::suspend_never final_suspend() noexcept { return {}; }  ///< 结束时不再二次挂起，由完成逻辑直接清理

    void unhandled_exception() noexcept  ///< 捕获协程异常并写入完成态
    {
        detail::storeTaskError(m_task, detail::TaskResultError(detail::TaskResultErrorCode::kTaskException));
        detail::completeTaskState(m_task);
    }

    template <typename U>
    void return_value(U&& value) noexcept(std::is_nothrow_constructible_v<T, U&&>)  ///< 写入协程返回值并标记完成
    {
        if (!detail::storeTaskResult<T>(m_task, std::forward<U>(value))) {
            detail::storeTaskError(m_task, detail::TaskResultError(detail::TaskResultErrorCode::kInvalidState));
        }
        detail::completeTaskState(m_task);
    }

    const TaskRef& taskRefView() const noexcept { return m_task; }  ///< 返回底层任务引用视图

private:
    TaskRef m_task;
};

/**
 * @brief `void` 任务的 promise 特化
 */
template <>
class TaskPromise<void>
{
public:
    using ReSchedulerType = bool;  ///< `co_yield true/false` 使用的重新调度标记类型

    int get_return_object_on_alloaction_failure() noexcept { return -1; }  ///< 协程分配失败时返回错误码占位

    Task<void> get_return_object() noexcept  ///< 构造并返回与该 promise 绑定的 Task
    {
        auto handle = std::coroutine_handle<TaskPromise<void>>::from_promise(*this);
        m_task = TaskRef(new TaskState(handle), false);
        detail::initializeTaskResult<void>(m_task);
        detail::inheritTaskRuntime(m_task, detail::currentRuntime());
        return Task<void>(m_task);
    }

    std::suspend_always initial_suspend() noexcept { return {}; }  ///< 初始总是挂起，交由调度器决定首次恢复时机

    std::suspend_always yield_value(ReSchedulerType flag) noexcept  ///< `co_yield true` 时把任务重新放回延后队列
    {
        if (flag) {
            detail::scheduleTaskDeferred(m_task);
        }
        return {};
    }

    std::suspend_never final_suspend() noexcept { return {}; }  ///< 结束时不再二次挂起，由完成逻辑直接清理

    void unhandled_exception() noexcept  ///< 捕获协程异常并写入完成态
    {
        detail::storeTaskError(m_task, detail::TaskResultError(detail::TaskResultErrorCode::kTaskException));
        detail::completeTaskState(m_task);
    }

    void return_void() noexcept  ///< 标记 `void` 协程成功完成
    {
        detail::completeTaskState(m_task);
    }

    const TaskRef& taskRefView() const noexcept { return m_task; }  ///< 返回底层任务引用视图

private:
    TaskRef m_task;
};

} // namespace galay::kernel

#endif // GALAY_KERNEL_TASK_H
