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

#include "../../galay-utils/common/defn.hpp"

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

namespace detail
{

struct TaskRefStorageAccess;  ///< 供固定容量调度 ring 在 TaskRef 与裸状态指针间转移所有权
struct ReadyEntry;  ///< 调度器 ready queue 内部使用的语言中立就绪项

/** @brief 完成与阻塞等待注册共享同一个原子状态，避免跨原子握手丢唤醒。 */
enum class TaskCompletionStatus : uint32_t {
    kPending,
    kWaiting,
    kDone,
};
static_assert(std::atomic<TaskCompletionStatus>::is_always_lock_free);

/**
 * @brief 内部任务结果消费错误类别。
 */
enum class TaskResultErrorCode : uint8_t {
    kInvalidState,       ///< TaskRef 没有关联有效 TaskState
    kTaskException,      ///< 协程执行过程中记录了异常
    kAlreadyConsumed,    ///< 任务结果已被 join/await/blockOn 消费
    kResultUnavailable,  ///< 任务完成但结果存储不可用
    kScheduleFailed,     ///< 子任务无法提交到 scheduler
    kResumeFailed        ///< 等待中的父任务无法重新提交到 owner scheduler
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
        static constexpr std::array<std::string_view, static_cast<size_t>(TaskResultErrorCode::kResumeFailed) + 1> kMessages = {{
            "task state is invalid",
            "task completed with an unhandled exception",
            "task result has already been consumed",
            "task completed without an available result",
            "task could not be scheduled for execution",
            "task could not be resumed on its owner scheduler"
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

Runtime* current_runtime() noexcept;  ///< 读取当前线程绑定的 Runtime，上下文不存在时返回 nullptr
Runtime* swap_current_runtime(Runtime* runtime) noexcept;  ///< 替换当前线程 Runtime 并返回旧值
/**
 * @brief 分配 C++ coroutine frame 的存储。
 * @param size 编译器请求的 frame 字节数
 * @param alignment 编译器请求的对齐；超出 max_align_t 时不进入 TLS 桶
 * @return 成功返回可用于该 frame 的存储；失败返回 nullptr，不抛异常
 * @note 小 frame 只归还当前释放线程的无锁 TLS 缓存；大 frame 或超对齐请求
 *       使用匹配的全局对齐分配。
 */
void* allocate_frame_storage(std::size_t size, std::size_t alignment) noexcept;
/**
 * @brief 释放由 allocate_frame_storage 返回的 frame 存储。
 * @param ptr 待释放地址；允许为 nullptr，非空值必须来自
 *        allocate_frame_storage() 且尚未释放
 * @param size 编译器提供的 frame 大小；0 表示 unsized delete。该值不参与
 *       recycler 桶选择，实际分配来源由 frame 的 provenance header 决定。
 * @param alignment 与分配路径匹配的对齐
 * @pre `ptr` 非空时必须仍指向 allocate_frame_storage() 返回的原始地址；任意
 *      外部指针都不满足该 helper 的释放契约。
 */
void release_frame_storage(void* ptr,
                         std::size_t size,
                         std::size_t alignment) noexcept;
/**
 * @brief 设置当前线程的 frame recycler 开关。
 * @param enabled true 使用 TLS recycler；false 直接走匹配的全局分配
 * @note 仅供边界测试和 benchmark 做 A/B 对照；默认开启。
 */
void set_frame_recycler_enabled_for_testing(bool enabled) noexcept;
/**
 * @brief 让当前线程的 frame 分配暂时失败。
 * @note 仅用于验证标准 allocation-failure hook；默认关闭。
 */
void set_frame_allocation_failure_for_testing(bool enabled) noexcept;
/**
 * @brief 让当前线程的 nothrow TaskState 分配暂时失败。
 * @note 仅用于验证 get_return_object() 的空状态分支；默认关闭。
 */
void set_task_state_allocation_failure_for_testing(bool enabled) noexcept;
/**
 * @brief 查询当前线程指定 frame 桶的缓存节点数。
 * @note 仅供边界测试和 benchmark 观测，不参与分配热路径。
 */
std::size_t frame_free_list_size_for_testing(std::size_t size,
                                        std::size_t alignment) noexcept;
bool schedule_task(const TaskRef& task) noexcept;  ///< 将任务按普通语义提交给其所属调度器
bool schedule_task_deferred(const TaskRef& task) noexcept;  ///< 将任务按延后语义提交给其所属调度器
bool schedule_task_deferred_state(TaskState* state) noexcept;  ///< 通过裸状态提交延后任务，供 promise 热路径使用
bool schedule_task_immediately(const TaskRef& task) noexcept;  ///< 在所属调度器线程上立即恢复任务
enum class TaskResumeResult : uint8_t {
    kAccepted,
    kAlreadyQueued,
    kRejected,
};
TaskResumeResult request_task_resume_state_detailed(TaskState* state) noexcept;
bool request_task_resume(const TaskRef& task) noexcept;  ///< 请求恢复已暂停任务；失败时返回 false
bool request_task_resume_state(TaskState* state) noexcept;  ///< 通过 owner scheduler 的无分配入口请求恢复；调用方必须持有有效引用
std::thread::id scheduler_thread_id(Scheduler* scheduler) noexcept;  ///< 查询调度器线程 ID；scheduler 为空时返回默认值
void complete_task_state(const TaskRef& task) noexcept;  ///< 标记任务完成并触发 continuation 清理
void complete_task_state(TaskState* state) noexcept;  ///< 通过裸状态标记任务完成，供 promise 热路径使用
void attach_task_continuation(const TaskRef& task, TaskRef next) noexcept;  ///< 为任务追加下一段 continuation
bool wait_task_completion(const TaskRef& task);  ///< 阻塞等待任务完成；无有效任务状态时返回 false
void store_task_error(const TaskRef& task, TaskResultError error) noexcept;  ///< 写入任务错误
bool destroy_task_frame(TaskState* state) noexcept;  ///< 销毁仍处于初始挂起态的 frame，已完成 frame 不重复销毁
struct TaskAccess;  ///< 供内核实现访问 Task 私有状态的辅助入口
template <typename T>
class TaskAwaiter;  ///< `co_await Task<T>` 使用的 awaiter
template <typename T>
void initialize_task_result(const TaskRef& task) noexcept;  ///< 初始化任务结果存储
template <typename T, typename U>
bool store_task_result(const TaskRef& task, U&& value);  ///< 写入任务结果
template <typename T>
std::expected<T, TaskResultError> try_take_task_result(const TaskRef& task);  ///< 以返回值消费任务结果

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

    bool is_valid() const noexcept { return m_state != nullptr; }  ///< 是否引用到有效任务状态
    TaskState* state() const noexcept
    {
        const auto raw = reinterpret_cast<uintptr_t>(m_state);
        return reinterpret_cast<TaskState*>(raw & ~kBorrowedBit);
    }  ///< 返回非拥有状态指针；promise view 使用低位标记
    Scheduler* belong_scheduler() const noexcept;  ///< 返回任务所属调度器；未绑定时返回 nullptr

private:
    template <typename T>
    friend class Task;
    template <typename T>
    friend class TaskPromise;
    friend struct detail::TaskRefStorageAccess;

    static TaskRef borrowed(TaskState* state) noexcept;
    bool is_borrowed() const noexcept
    {
        return (reinterpret_cast<uintptr_t>(m_state) & kBorrowedBit) != 0;
    }

    void retain() noexcept;  ///< 增加底层状态引用计数
    void release() noexcept;  ///< 减少底层状态引用计数，必要时释放状态

    static constexpr uintptr_t kBorrowedBit = 1U;
    TaskState* m_state = nullptr;
};

/**
 * @brief 协程任务的共享状态块
 * @details 保存 coroutine handle、所属调度器、Runtime 上下文和 continuation 链。
 */
struct alignas(::galay::utils::kCacheLineSize) TaskState
{
    static constexpr size_t kInlineResultBytes = 32;
    enum class ResultStorageKind : uint8_t { Empty, Inline, Heap };

    template <typename Promise>
    explicit TaskState(std::coroutine_handle<Promise> handle) noexcept
        : m_handle(handle) {}

    ~TaskState();

    static void* operator new(std::size_t size);
    static void* operator new(std::size_t size, std::align_val_t alignment);
    static void* operator new(std::size_t size, const std::nothrow_t&) noexcept;
    static void* operator new(std::size_t size,
                              std::align_val_t alignment,
                              const std::nothrow_t&) noexcept;
    static void operator delete(void* ptr) noexcept;
    static void operator delete(void* ptr, std::size_t size) noexcept;
    static void operator delete(void* ptr, std::align_val_t alignment) noexcept;
    static void operator delete(void* ptr, std::size_t size, std::align_val_t alignment) noexcept;

    void* result_storage() noexcept { return static_cast<void*>(m_result_storage); }
    const void* result_storage() const noexcept { return static_cast<const void*>(m_result_storage); }
    /** @brief 只有终态 kDone 表示完成，阻塞等待注册不影响调度资格。 */
    bool is_done(std::memory_order order = std::memory_order_acquire) const noexcept
    {
        return m_completion_status.load(order) == detail::TaskCompletionStatus::kDone;
    }

    alignas(std::max_align_t) std::byte m_result_storage[kInlineResultBytes]{};  ///< 小对象内联结果存储
    std::coroutine_handle<> m_handle = nullptr;  ///< 底层协程句柄
    Scheduler* m_scheduler = nullptr;  ///< 任务所属调度器
    Runtime* m_runtime = nullptr;  ///< 任务继承到的 Runtime 上下文
    TaskState* m_resume_queue_next = nullptr;  ///< owner scheduler 专用侵入式 resume 链接
    // 将所有权/接纳标记放在冷结果和 continuation 字段之前；在保持 128 字节
    // 状态大小的同时，减少热路径跨越的缓存行。
    std::atomic<uint64_t> m_refs{1};  ///< TaskRef 引用计数
    // 使用 32 位状态，使支持 futex 的标准库可直接等待此地址，避免 bool 等待的
    // 共享版本计数；kWaiting 仍表示任务未完成，只有 kDone 发布结果。
    std::atomic<detail::TaskCompletionStatus> m_completion_status{detail::TaskCompletionStatus::kPending};
    std::atomic<bool> m_queued{false};  ///< 任务是否已在调度队列中
    std::atomic<bool> m_resume_queue_claimed{false};  ///< 是否已被 resume admission 接管
    std::atomic<bool> m_resume_owner_only{false};  ///< 是否必须由 owner scheduler 线程恢复
    std::atomic<bool> m_result_consumed{false};  ///< 任务结果是否已被 join/await 消费
    std::optional<detail::TaskResultError> m_result_error;  ///< 任务错误
    ResultStorageKind m_result_kind = ResultStorageKind::Empty;  ///< 当前结果存储形态
    void (*m_destroy_result)(TaskState&) noexcept = nullptr;  ///< 销毁尚未消费的结果对象
    std::optional<TaskRef> m_then;  ///< `then()` 追加的 continuation 任务
    std::optional<TaskRef> m_next;  ///< 当前 `co_await` 后要恢复的父任务
};

// Keep ownership transfers visible to CRTP callers so moved-from cleanup and
// forwarding disappear without LTO. Reference counts and borrowed views retain
// the same lifetime contract; actual frame/state destruction stays out of line.
inline TaskRef::TaskRef(TaskState* state, bool retainRef) noexcept
    : m_state(state)
{
    if (retainRef) {
        retain();
    }
}

inline TaskRef TaskRef::borrowed(TaskState* state) noexcept
{
    TaskRef result;
    if (state == nullptr) {
        return result;
    }
    const auto raw = reinterpret_cast<uintptr_t>(state);
    result.m_state = reinterpret_cast<TaskState*>(raw | kBorrowedBit);
    return result;
}

inline TaskRef::TaskRef(const TaskRef& other) noexcept
    : m_state(other.state())
{
    retain();
}

inline TaskRef::TaskRef(TaskRef&& other) noexcept
    : m_state(other.m_state)
{
    other.m_state = nullptr;
}

inline TaskRef::~TaskRef()
{
    release();
}

inline TaskRef& TaskRef::operator=(const TaskRef& other) noexcept
{
    if (this != &other) {
        auto* state = other.state();
        if (state != nullptr) {
            // Retain before release: other may be this state's borrowed view.
            state->m_refs.fetch_add(1, std::memory_order_relaxed);
        }
        release();
        m_state = state;
    }
    return *this;
}

inline TaskRef& TaskRef::operator=(TaskRef&& other) noexcept
{
    if (this != &other) {
        if (other.is_borrowed() && state() == other.state()) {
            // A borrowed promise view cannot replace its own owning reference.
            other.m_state = nullptr;
            return *this;
        }
        release();
        m_state = other.m_state;
        other.m_state = nullptr;
    }
    return *this;
}

inline Scheduler* TaskRef::belong_scheduler() const noexcept
{
    auto* state = this->state();
    return state ? state->m_scheduler : nullptr;
}

inline void TaskRef::retain() noexcept
{
    if (auto* state = this->state()) {
        state->m_refs.fetch_add(1, std::memory_order_relaxed);
    }
}

inline void TaskRef::release() noexcept
{
    if (!m_state) {
        return;
    }
    if (is_borrowed()) {
        m_state = nullptr;
        return;
    }
    auto* state = this->state();
    m_state = nullptr;
    if (state->m_refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        delete state;
    }
}

namespace detail
{

inline Runtime* task_runtime(const TaskRef& task) noexcept
{
    auto* state = task.state();
    return state ? state->m_runtime : nullptr;
}

inline void set_task_runtime(const TaskRef& task, Runtime* runtime) noexcept
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

inline void inherit_task_runtime(TaskState* state, Runtime* runtime) noexcept
{
    if (state != nullptr && state->m_runtime == nullptr) {
        state->m_runtime = runtime;
    }
}

inline void inherit_task_runtime(const TaskRef& task, Runtime* runtime) noexcept
{
    inherit_task_runtime(task.state(), runtime);
}

inline void set_task_scheduler(const TaskRef& task, Scheduler* scheduler) noexcept
{
    if (auto* state = task.state()) {
        state->m_scheduler = scheduler;
        if (state->m_then.has_value() && state->m_then->belong_scheduler() == nullptr) {
            set_task_scheduler(*state->m_then, scheduler);
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
        : m_previous(swap_current_runtime(runtime)) {}

    ~CurrentRuntimeScope()
    {
        swap_current_runtime(m_previous);
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
void initialize_task_result(const TaskRef& task) noexcept
{
    if (auto* state = task.state()) {
        state->m_destroy_result = &TaskResultStorageTraits<T>::destroy;
    }
}

template <typename T>
void initialize_task_result(TaskState* state) noexcept
{
    if (state != nullptr) {
        state->m_destroy_result = &TaskResultStorageTraits<T>::destroy;
    }
}

template <>
inline void initialize_task_result<void>(const TaskRef& task) noexcept
{
    if (auto* state = task.state()) {
        state->m_destroy_result = nullptr;
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
bool store_task_result(const TaskRef& task, U&& value)
{
    auto* state = task.state();
    if (state == nullptr) {
        return false;
    }
    TaskResultStorageTraits<T>::store(*state, std::forward<U>(value));
    return true;
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
     */
    template <typename U>
    void set_value(U&& value)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_value = std::forward<U>(value);
            m_ready = true;
        }
        m_cv.notify_all();
    }

    /**
     * @brief 写入任务错误并唤醒等待者
     * @param error 要返回给 join() 的任务错误
     */
    void set_error(detail::TaskResultError error)
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_error = std::move(error);
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
        if (m_error.has_value()) {
            m_consumed = true;
            return std::unexpected(*m_error);
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
    std::optional<detail::TaskResultError> m_error;
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
    void set_value()  ///< 标记任务成功完成并唤醒等待者
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_ready = true;
        }
        m_cv.notify_all();
    }

    void set_error(detail::TaskResultError error)  ///< 写入任务错误并唤醒等待者
    {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_error = std::move(error);
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
        if (m_error.has_value()) {
            m_consumed = true;
            return std::unexpected(*m_error);
        }
        m_consumed = true;
        return {};
    }

private:
    mutable std::mutex m_mutex;
    mutable std::condition_variable m_cv;
    std::optional<detail::TaskResultError> m_error;
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

    bool is_valid() const { return m_task.is_valid(); }  ///< 是否持有可用任务
    bool done() const
    {
        auto* state = m_task.state();
        return !state || state->is_done();
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

    std::expected<T, detail::TaskResultError> take_result()  ///< 取走任务结果；失败时返回 TaskResultError
    {
        return detail::try_take_task_result<T>(m_task);
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

    bool is_valid() const { return m_task.is_valid(); }  ///< 是否持有可用任务
    bool done() const
    {
        auto* state = m_task.state();
        return !state || state->is_done();
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

    std::expected<void, detail::TaskResultError> take_result()  ///< 消费完成状态；失败时返回 TaskResultError
    {
        return detail::try_take_task_result<void>(m_task);
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

    bool is_valid() const noexcept { return m_task.is_valid() || static_cast<bool>(m_blocking_completion); }  ///< 是否绑定到有效任务完成态

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

    bool is_valid() const noexcept { return m_task.is_valid() || static_cast<bool>(m_blocking_completion); }  ///< 是否绑定到有效任务完成态

    std::expected<void, detail::TaskResultError> wait() const  ///< 阻塞等待任务结束，不消费完成状态
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

    std::expected<void, detail::TaskResultError> join()  ///< 阻塞等待并消费完成状态
    {
        if (m_task.is_valid()) {
            return detail::try_take_task_result<void>(m_task);
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

struct TaskRefStorageAccess
{
    static TaskState* release_state(TaskRef& task) noexcept
    {
        if (task.is_borrowed()) {
            task.m_state = nullptr;
            return nullptr;
        }
        TaskState* state = task.state();
        task.m_state = nullptr;
        return state;
    }

    static TaskRef adopt_state(TaskState* state) noexcept
    {
        return TaskRef(state, false);
    }
};

/**
 * @brief 多生产者、单 owner 消费者的无分配任务恢复队列。
 * @details 每个已停泊任务通过 TaskState 内嵌链接进入栈；owner 一次摘取整条链后
 *          反转为近似 FIFO。push 成功即接管传入 TaskRef 的唯一引用。
 * @note 同一 TaskState 在被 owner 摘取前最多只能入队一次；该不变量由
 *       request_task_resume_state() 的 m_queued 仲裁保证。
 */
class TaskResumeQueue
{
public:
    TaskResumeQueue() noexcept = default;
    TaskResumeQueue(const TaskResumeQueue&) = delete;
    TaskResumeQueue& operator=(const TaskResumeQueue&) = delete;

    ~TaskResumeQueue()
    {
        close();
        release_all(take_all());
    }

    [[nodiscard]] bool push(TaskRef task) noexcept
    {
        TaskState* state = TaskRefStorageAccess::release_state(task);
        if (state == nullptr) {
            return false;
        }

        bool expected = false;
        if (state->m_resume_queue_claimed.load(std::memory_order_acquire) ||
            !state->m_resume_queue_claimed.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            [[maybe_unused]] TaskRef rejected =
                TaskRefStorageAccess::adopt_state(state);
            return false;
        }

        uintptr_t head = m_head.load(std::memory_order_acquire);
        for (;;) {
            if ((head & kClosedBit) != 0) {
                state->m_resume_queue_next = nullptr;
                state->m_resume_queue_claimed.store(false,
                                                    std::memory_order_release);
                [[maybe_unused]] TaskRef rejected =
                    TaskRefStorageAccess::adopt_state(state);
                return false;
            }
            state->m_resume_queue_next = decode(head);
            const uintptr_t desired = encode(state);
            if (m_head.compare_exchange_weak(
                    head,
                    desired,
                    std::memory_order_release,
                    std::memory_order_acquire)) {
                return true;
            }
        }
    }

    [[nodiscard]] TaskState* take_all() noexcept
    {
        uintptr_t head = m_head.load(std::memory_order_acquire);
        // Empty drain linearizes at this acquire observation; later pushes stay queued.
        if (decode(head) == nullptr) {
            return nullptr;
        }
        for (;;) {
            const uintptr_t replacement = head & kClosedBit;
            if (m_head.compare_exchange_weak(
                    head,
                    replacement,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return decode(head);
            }
        }
    }

    [[nodiscard]] bool empty() const noexcept
    {
        return decode(m_head.load(std::memory_order_acquire)) == nullptr;
    }

    void close() noexcept
    {
        const uintptr_t previous =
            m_head.fetch_or(kClosedBit, std::memory_order_acq_rel);
        if ((previous & kClosedBit) != 0) {
            return;
        }
    }

    [[nodiscard]] bool reopen() noexcept
    {
        uintptr_t state = m_head.load(std::memory_order_acquire);
        if (state == 0) {
            return true;
        }
        if (state != kClosedBit) {
            return false;
        }
        return m_head.compare_exchange_strong(
            state,
            0,
            std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    [[nodiscard]] bool is_closed() const noexcept
    {
        return (m_head.load(std::memory_order_acquire) & kClosedBit) != 0;
    }

    [[nodiscard]] static TaskState* reverse(TaskState* head) noexcept
    {
        TaskState* reversed = nullptr;
        while (head != nullptr) {
            TaskState* next = head->m_resume_queue_next;
            head->m_resume_queue_next = reversed;
            reversed = head;
            head = next;
        }
        return reversed;
    }

    [[nodiscard]] static TaskRef pop_front(TaskState*& head) noexcept
    {
        if (head == nullptr) {
            return {};
        }
        TaskState* state = head;
        head = state->m_resume_queue_next;
        state->m_resume_queue_next = nullptr;
        return TaskRefStorageAccess::adopt_state(state);
    }

    static void release_all(TaskState* head) noexcept
    {
        while (head != nullptr) {
            TaskRef released = pop_front(head);
            if (TaskState* state = released.state(); state != nullptr) {
                state->m_resume_queue_claimed.store(false,
                                                    std::memory_order_release);
            }
        }
    }

private:
    static constexpr uintptr_t kClosedBit = 1;

    static uintptr_t encode(TaskState* state) noexcept
    {
        return reinterpret_cast<uintptr_t>(state);
    }

    static TaskState* decode(uintptr_t state) noexcept
    {
        return reinterpret_cast<TaskState*>(state & ~kClosedBit);
    }

    static_assert(alignof(TaskState) >= 2,
                  "TaskState alignment must leave one pointer tag bit free");

    std::atomic<uintptr_t> m_head{0};
};

/**
 * @brief ready queue 中的语言中立就绪项类别。
 * @details C++ 任务使用 TaskState；C 协程任务通过 ReadyEntryCoroHeader 暴露调度 hook。
 */
enum class ReadyEntryKind : uintptr_t {
    Empty = 0,
    CppTask = 1,
    CCoroutine = 2,
};

/**
 * @brief C 协程 ready entry 的 hook 表。
 * @details C 协程状态对象必须以 ReadyEntryCoroHeader 作为首字段，使调度器可用
 *          void* state 查询 owner、owner-only 约束、恢复入口和 ready 引用释放入口。
 */
struct ReadyEntryHooks
{
    Scheduler* (*owner_scheduler)(void* state) noexcept = nullptr;  ///< 返回 owner scheduler
    bool (*resume_owner_only)(void* state) noexcept = nullptr;  ///< 是否只能由 owner scheduler 恢复
    bool (*resume)(void* state) noexcept = nullptr;  ///< 恢复 C 协程；成功后 ready entry 会释放
    void (*release)(void* state) noexcept = nullptr;  ///< 释放 ready entry 持有的 C 协程引用
};

/**
 * @brief C 协程 ready 状态首字段。
 * @note 该结构不拥有 hooks，只保存静态 hook 表指针。
 */
struct ReadyEntryCoroHeader
{
    const ReadyEntryHooks* hooks = nullptr;
};

/**
 * @brief 调度器 ready queue 的低开销就绪项。
 * @details kind 被打包进 state 指针低位，避免热路径 std::function、堆分配或虚调用。
 *          ReadyEntry 拥有一个 ready 引用，因此只允许移动；析构会释放尚未转移的引用。
 *          被入队的 state 必须至少 4 字节对齐；TaskState 已按 64 字节对齐。
 */
struct ReadyEntry
{
    static constexpr uintptr_t kKindMask = 0x3U;

    constexpr ReadyEntry() noexcept = default;

    ReadyEntry(const ReadyEntry&) = delete;
    ReadyEntry& operator=(const ReadyEntry&) = delete;

    ReadyEntry(ReadyEntry&& other) noexcept
        : m_encoded(other.m_encoded)
    {
        other.m_encoded = 0;
    }

    ReadyEntry& operator=(ReadyEntry&& other) noexcept
    {
        if (this != &other) {
            reset();
            m_encoded = other.m_encoded;
            other.m_encoded = 0;
        }
        return *this;
    }

    ~ReadyEntry()
    {
        reset();
    }

    explicit ReadyEntry(TaskRef&& task) noexcept
        : ReadyEntry(ReadyEntryKind::CppTask, TaskRefStorageAccess::release_state(task))
    {
    }

    ReadyEntry(ReadyEntryKind kind, void* state) noexcept
    {
        if (state == nullptr || kind == ReadyEntryKind::Empty) {
            m_encoded = 0;
            return;
        }
        const auto raw = reinterpret_cast<uintptr_t>(state);
        if ((raw & kKindMask) != 0) {
            m_encoded = 0;
            return;
        }
        m_encoded = (raw & ~kKindMask) | static_cast<uintptr_t>(kind);
    }

    static ReadyEntry from_encoded(uintptr_t encoded) noexcept
    {
        ReadyEntry entry;
        entry.m_encoded = encoded;
        return entry;
    }

    bool is_valid() const noexcept { return m_encoded != 0; }
    ReadyEntryKind kind() const noexcept
    {
        return static_cast<ReadyEntryKind>(m_encoded & kKindMask);
    }
    bool is_cpp_task() const noexcept { return kind() == ReadyEntryKind::CppTask; }
    void* state() const noexcept
    {
        return reinterpret_cast<void*>(m_encoded & ~kKindMask);
    }
    TaskState* task_state() const noexcept
    {
        return is_cpp_task() ? static_cast<TaskState*>(state()) : nullptr;
    }
    uintptr_t encoded() const noexcept { return m_encoded; }
    void clear() noexcept { m_encoded = 0; }
    void reset() noexcept
    {
        if (m_encoded == 0) {
            return;
        }

        const auto current_kind = kind();
        void* current_state = state();
        m_encoded = 0;

        if (current_kind == ReadyEntryKind::CppTask) {
            [[maybe_unused]] TaskRef released =
                TaskRefStorageAccess::adopt_state(static_cast<TaskState*>(current_state));
            return;
        }

        if (current_kind == ReadyEntryKind::CCoroutine && current_state != nullptr) {
            auto* header = static_cast<ReadyEntryCoroHeader*>(current_state);
            if (header->hooks != nullptr && header->hooks->release != nullptr) {
                header->hooks->release(current_state);
            }
        }
    }

private:
    uintptr_t m_encoded = 0;
};

inline ReadyEntry ready_entry_from_task_ref(TaskRef&& task) noexcept
{
    return ReadyEntry(std::move(task));
}

inline TaskRef ready_entry_to_task_ref(ReadyEntry& entry) noexcept
{
    if (!entry.is_cpp_task()) {
        return {};
    }
    auto* state = entry.task_state();
    entry.clear();
    return TaskRefStorageAccess::adopt_state(state);
}

inline const ReadyEntryHooks* ready_entry_hooks(const ReadyEntry& entry) noexcept
{
    if (entry.kind() != ReadyEntryKind::CCoroutine || entry.state() == nullptr) {
        return nullptr;
    }
    return static_cast<ReadyEntryCoroHeader*>(entry.state())->hooks;
}

inline void release_ready_entry(ReadyEntry& entry) noexcept
{
    entry.reset();
}

inline Scheduler* ready_entry_scheduler(const ReadyEntry& entry) noexcept
{
    if (auto* state = entry.task_state()) {
        return state->m_scheduler;
    }
    const auto* hooks = ready_entry_hooks(entry);
    return hooks != nullptr && hooks->owner_scheduler != nullptr
        ? hooks->owner_scheduler(entry.state())
        : nullptr;
}

inline bool ready_entry_resume_owner_only(const ReadyEntry& entry) noexcept
{
    if (auto* state = entry.task_state()) {
        return state->m_resume_owner_only.load(std::memory_order_acquire);
    }
    const auto* hooks = ready_entry_hooks(entry);
    return hooks != nullptr && hooks->resume_owner_only != nullptr &&
        hooks->resume_owner_only(entry.state());
}

inline bool resume_task_state(TaskState* state)
{
    if (!state || !state->m_handle || state->is_done(std::memory_order_relaxed)) {
        return false;
    }
    state->m_queued.store(false, std::memory_order_relaxed);
    state->m_resume_queue_claimed.store(false, std::memory_order_release);
    state->m_resume_owner_only.store(false, std::memory_order_relaxed);
    if (state->m_runtime == nullptr || state->m_runtime == current_runtime()) {
        state->m_handle.resume();
        return true;
    }
    CurrentRuntimeScope runtime_scope(state->m_runtime);
    state->m_handle.resume();
    return true;
}

inline bool resume_ready_entry(ReadyEntry& entry)
{
    if (!entry.is_valid()) {
        return false;
    }

    if (entry.is_cpp_task()) {
        TaskRef task = ready_entry_to_task_ref(entry);
        return resume_task_state(task.state());
    }

    const auto* hooks = ready_entry_hooks(entry);
    if (hooks == nullptr || hooks->resume == nullptr) {
        return false;
    }
    const bool resumed = hooks->resume(entry.state());
    if (resumed) {
        release_ready_entry(entry);
    }
    return resumed;
}

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
        TaskRef childTask = TaskAccess::task_ref(m_task);
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
            childTask.state()->m_next.reset();
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
    detail::attach_task_continuation(m_task, detail::TaskAccess::detach_task(std::move(next)));
    return *this;
}

inline Task<void>&& Task<void>::then(Task<void> next) &&  ///< 为当前任务追加 continuation，返回右值引用
{
    detail::attach_task_continuation(m_task, detail::TaskAccess::detach_task(std::move(next)));
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

    static Task<T> get_return_object_on_allocation_failure() noexcept
    {
        return {};
    }  ///< 协程帧分配失败时返回无效任务

    Task<T> get_return_object() noexcept  ///< 构造并返回与该 promise 绑定的 Task
    {
        auto handle = std::coroutine_handle<TaskPromise<T>>::from_promise(*this);
        auto* state = new (std::nothrow) TaskState(handle);
        if (state == nullptr) {
            return {};
        }
        m_state = state;
        m_task_view = TaskRef::borrowed(state);
        detail::initialize_task_result<T>(state);
        detail::inherit_task_runtime(state, detail::current_runtime());
        return Task<T>(TaskRef(state, false));
    }

    struct InitialSuspendAwaiter
    {
        TaskPromise* promise = nullptr;

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

    InitialSuspendAwaiter initial_suspend() noexcept { return {this}; }  ///< 初始总是挂起，失败状态在挂起点释放 frame

    std::suspend_always yield_value(ReSchedulerType flag) noexcept  ///< `co_yield true` 时把任务重新放回延后队列
    {
        if (flag) {
            detail::schedule_task_deferred_state(m_state);
        }
        return {};
    }

    std::suspend_never final_suspend() noexcept { return {}; }  ///< 结束时不再二次挂起，由完成逻辑直接清理

    void unhandled_exception() noexcept  ///< 捕获协程异常并写入完成态
    {
        detail::store_task_error(m_state, detail::TaskResultError(detail::TaskResultErrorCode::kTaskException));
        detail::complete_task_state(m_state);
    }

    template <typename U>
    void return_value(U&& value) noexcept(std::is_nothrow_constructible_v<T, U&&>)  ///< 写入协程返回值并标记完成
    {
        if (!detail::store_task_result<T>(m_state, std::forward<U>(value))) {
            detail::store_task_error(m_state, detail::TaskResultError(detail::TaskResultErrorCode::kInvalidState));
        }
        detail::complete_task_state(m_state);
    }

    ~TaskPromise() noexcept
    {
        if (m_state != nullptr) {
            // 编译器和显式调用 coroutine_handle::destroy() 都可能在不进入
            // TaskState::~TaskState() 的情况下释放 frame。清除非拥有的 handle view，
            // 避免后续状态清理再次尝试销毁 frame。
            m_state->m_handle = nullptr;
        }
        m_state = nullptr;
        m_task_view.m_state = nullptr;
    }

    const TaskRef& task_ref_view() const noexcept { return m_task_view; }  ///< 返回非拥有 TaskRef view

private:
    TaskState* m_state = nullptr;  ///< promise 热路径使用的非拥有状态指针
    mutable TaskRef m_task_view;  ///< 为兼容 awaiter 保留的非拥有 TaskRef view
};

/**
 * @brief `void` 任务的 promise 特化
 */
template <>
class TaskPromise<void>
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

    static Task<void> get_return_object_on_allocation_failure() noexcept
    {
        return {};
    }  ///< 协程帧分配失败时返回无效任务

    Task<void> get_return_object() noexcept  ///< 构造并返回与该 promise 绑定的 Task
    {
        auto handle = std::coroutine_handle<TaskPromise<void>>::from_promise(*this);
        auto* state = new (std::nothrow) TaskState(handle);
        if (state == nullptr) {
            return {};
        }
        m_state = state;
        m_task_view = TaskRef::borrowed(state);
        detail::initialize_task_result<void>(state);
        detail::inherit_task_runtime(state, detail::current_runtime());
        return Task<void>(TaskRef(state, false));
    }

    struct InitialSuspendAwaiter
    {
        TaskPromise* promise = nullptr;

        bool await_ready() const noexcept { return false; }

        bool await_suspend(std::coroutine_handle<> handle) const noexcept
        {
            if (promise->m_state == nullptr) {
                // 参见 TaskPromise<T>::InitialSuspendAwaiter。
                handle.destroy();
            }
            return true;
        }

        void await_resume() const noexcept {}
    };

    InitialSuspendAwaiter initial_suspend() noexcept { return {this}; }  ///< 初始总是挂起，失败状态在挂起点释放 frame

    std::suspend_always yield_value(ReSchedulerType flag) noexcept  ///< `co_yield true` 时把任务重新放回延后队列
    {
        if (flag) {
            detail::schedule_task_deferred_state(m_state);
        }
        return {};
    }

    std::suspend_never final_suspend() noexcept { return {}; }  ///< 结束时不再二次挂起，由完成逻辑直接清理

    void unhandled_exception() noexcept  ///< 捕获协程异常并写入完成态
    {
        detail::store_task_error(m_state, detail::TaskResultError(detail::TaskResultErrorCode::kTaskException));
        detail::complete_task_state(m_state);
    }

    void return_void() noexcept  ///< 标记 `void` 协程成功完成
    {
        detail::complete_task_state(m_state);
    }

    ~TaskPromise() noexcept
    {
        if (m_state != nullptr) {
            // 参见 TaskPromise<T>::~TaskPromise()。
            m_state->m_handle = nullptr;
        }
        m_state = nullptr;
        m_task_view.m_state = nullptr;
    }

    const TaskRef& task_ref_view() const noexcept { return m_task_view; }  ///< 返回非拥有 TaskRef view

private:
    TaskState* m_state = nullptr;  ///< promise 热路径使用的非拥有状态指针
    mutable TaskRef m_task_view;  ///< 为兼容 awaiter 保留的非拥有 TaskRef view
};

} // namespace galay::kernel

#endif // GALAY_KERNEL_TASK_H
