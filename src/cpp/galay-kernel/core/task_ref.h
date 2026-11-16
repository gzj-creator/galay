/**
 * @file task_ref.h
 * @brief 任务共享状态与轻量引用
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 定义 TaskState、TaskRef 及其运行时、调度器绑定辅助函数。该头只依赖
 *          协程句柄和原子状态，不引入协程 Task 与调度器 ready queue 的内部类型。
 */

#ifndef GALAY_KERNEL_TASK_REF_H
#define GALAY_KERNEL_TASK_REF_H

#include "../../galay-utils/common/cache_line.hpp"

#include <array>
#include <atomic>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <new>
#include <string_view>

namespace galay::kernel
{

class Scheduler;
class Runtime;
template <typename T>
class TaskPromise;
template <typename T>
class Task;
struct TaskState;
class TaskRef;

namespace detail
{

struct TaskRefStorageAccess;

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

    /**
     * @brief 返回任务结果消费错误类别
     * @return 当前对象的类型、状态或错误码
     */
    TaskResultErrorCode code() const noexcept { return m_code; }
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

/**
 * @brief 读取当前线程绑定的 Runtime，上下文不存在时返回 nullptr
 * @return Runtime* 指针
 */
Runtime* current_runtime() noexcept;
/**
 * @brief 替换当前线程 Runtime 并返回旧值
 * @param runtime 运行时
 * @return Runtime* 指针
 */
Runtime* swap_current_runtime(Runtime* runtime) noexcept;
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
 * @return 无返回值
 */
void release_frame_storage(void* ptr,
                         std::size_t size,
                         std::size_t alignment) noexcept;
/**
 * @brief 设置当前线程的 frame recycler 开关。
 * @param enabled true 使用 TLS recycler；false 直接走匹配的全局分配
 * @return 无返回值
 * @note 仅供边界测试和 benchmark 做 A/B 对照；默认开启。
 */
void set_frame_recycler_enabled_for_testing(bool enabled) noexcept;
/**
 * @brief 让当前线程的 frame 分配暂时失败。
 * @param enabled 是否启用
 * @return 无返回值
 * @note 仅用于验证标准 allocation-failure hook；默认关闭。
 */
void set_frame_allocation_failure_for_testing(bool enabled) noexcept;
/**
 * @brief 让当前线程的 nothrow TaskState 分配暂时失败。
 * @param enabled 是否启用
 * @return 无返回值
 * @note 仅用于验证 get_return_object() 的空状态分支；默认关闭。
 */
void set_task_state_allocation_failure_for_testing(bool enabled) noexcept;
/**
 * @brief 查询当前线程指定 frame 桶的缓存节点数。
 * @param size 大小
 * @param alignment 内存对齐字节数
 * @return 当前线程指定协程帧桶的空闲节点数量
 * @note 仅供边界测试和 benchmark 观测，不参与分配热路径。
 */
std::size_t frame_free_list_size_for_testing(std::size_t size,
                                        std::size_t alignment) noexcept;
/**
 * @brief 将任务按普通语义提交给其所属调度器
 * @param task 协程任务
 * @return 操作成功时返回 true，否则返回 false
 */
bool schedule_task(const TaskRef& task) noexcept;
/**
 * @brief 将任务按延后语义提交给其所属调度器
 * @param task 协程任务
 * @return 操作成功时返回 true，否则返回 false
 */
bool schedule_task_deferred(const TaskRef& task) noexcept;
/**
 * @brief 通过裸状态提交延后任务，供 promise 热路径使用
 * @param state 状态对象
 * @return 操作成功时返回 true，否则返回 false
 */
bool schedule_task_deferred_state(TaskState* state) noexcept;
/**
 * @brief 在所属调度器线程上立即恢复任务
 * @param task 协程任务
 * @return 操作成功时返回 true，否则返回 false
 */
bool schedule_task_immediately(const TaskRef& task) noexcept;
enum class TaskResumeResult : uint8_t {
    kAccepted,
    kAlreadyQueued,
    kRejected,
};
TaskResumeResult request_task_resume_state_detailed(TaskState* state) noexcept;
/**
 * @brief 请求恢复已暂停任务；失败时返回 false
 * @param task 协程任务
 * @return 操作成功时返回 true，否则返回 false
 */
bool request_task_resume(const TaskRef& task) noexcept;
/**
 * @brief 通过 owner scheduler 的无分配入口请求恢复；调用方必须持有有效引用
 * @param state 状态对象
 * @return 操作成功时返回 true，否则返回 false
 */
bool request_task_resume_state(TaskState* state) noexcept;
/**
 * @brief 标记任务完成并触发 continuation 清理
 * @param task 协程任务
 * @return 无返回值
 */
void complete_task_state(const TaskRef& task) noexcept;
/**
 * @brief 通过裸状态标记任务完成，供 promise 热路径使用
 * @param state 状态对象
 * @return 无返回值
 */
void complete_task_state(TaskState* state) noexcept;
/**
 * @brief 为任务追加下一段 continuation
 * @param task 协程任务
 * @param next 后继对象
 * @return 无返回值
 */
void attach_task_continuation(const TaskRef& task, TaskRef next) noexcept;
/**
 * @brief 阻塞等待任务完成；无有效任务状态时返回 false
 * @param task 协程任务
 * @return 操作成功时返回 true，否则返回 false
 */
bool wait_task_completion(const TaskRef& task);
/**
 * @brief 写入任务错误
 * @param task 协程任务
 * @param error 错误信息
 * @return 无返回值
 */
void store_task_error(const TaskRef& task, TaskResultError error) noexcept;
/**
 * @brief 销毁仍处于初始挂起态的 frame，已完成 frame 不重复销毁
 * @param state 状态对象
 * @return 操作成功时返回 true，否则返回 false
 */
bool destroy_task_frame(TaskState* state) noexcept;
struct TaskAccess;  ///< 供内核实现访问 Task 私有状态的辅助入口
template <typename T>
class TaskAwaiter;  ///< `co_await Task<T>` 使用的 awaiter
/**
 * @brief 以返回值消费任务结果
 * @param task 协程任务
 * @return 成功时返回 T，失败时返回 TaskResultError 错误
 */
template <typename T>
std::expected<T, TaskResultError> try_take_task_result(const TaskRef& task);

} // namespace detail

/**
 * @brief 任务状态的轻量引用句柄
 * @details 采用引用计数管理底层 `TaskState` 生命周期，可跨调度器和 continuation 链传递。
 */
class TaskRef
{
public:
    TaskRef() noexcept = default;
    /**
     * @brief 从裸状态创建引用；`retainRef=true` 时增加引用计数
     * @param state 状态对象
     * @param retainRef 是否保留任务引用
     */
    explicit TaskRef(TaskState* state, bool retainRef) noexcept;
    /**
     * @brief 拷贝并共享同一底层状态
     * @param other 源对象
     */
    TaskRef(const TaskRef& other) noexcept;
    /**
     * @brief 移动任务引用，源对象被清空
     * @param other 源对象
     */
    TaskRef(TaskRef&& other) noexcept;
    ~TaskRef();  ///< 释放引用；最后一个引用会回收底层状态

    /**
     * @brief 拷贝赋值并共享同一底层状态
     * @param other 源对象
     * @return 当前对象引用
     */
    TaskRef& operator=(const TaskRef& other) noexcept;
    /**
     * @brief 移动赋值，源对象被清空
     * @param other 源对象
     * @return 当前对象引用
     */
    TaskRef& operator=(TaskRef&& other) noexcept;

    /**
     * @brief 是否引用到有效任务状态
     * @return 满足所检查条件时返回 true，否则返回 false
     */
    bool is_valid() const noexcept { return m_state != nullptr; }
    /**
     * @brief 返回非拥有状态指针；promise view 使用低位标记
     * @return 当前对象的类型、状态或错误码
     */
    TaskState* state() const noexcept
    {
        const auto raw = reinterpret_cast<uintptr_t>(m_state);
        return reinterpret_cast<TaskState*>(raw & ~kBorrowedBit);
    }
    /**
     * @brief 返回任务所属调度器；未绑定时返回 nullptr
     * @return Scheduler* 指针
     */
    Scheduler* belong_scheduler() const noexcept;

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

    /**
     * @brief 增加底层状态引用计数
     * @return 无返回值
     */
    void retain() noexcept;
    /**
     * @brief 减少底层状态引用计数，必要时释放状态
     * @return 无返回值
     */
    void release() noexcept;

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
    /**
     * @brief 只有终态 kDone 表示完成，阻塞等待注册不影响调度资格。
     * @param order 原子操作内存顺序
     * @return 满足所检查条件时返回 true，否则返回 false
     */
    bool is_done(std::memory_order order = std::memory_order_acquire) const noexcept
    {
        return m_completion_status.load(order) == detail::TaskCompletionStatus::kDone;
    }

    /** @brief 返回当前任务结果的存储形态。 */
    ResultStorageKind result_storage_kind() const noexcept
    {
        return static_cast<ResultStorageKind>(
            m_result_state.load(std::memory_order_relaxed) &
            kResultStorageKindMask);
    }

    /**
     * @brief 更新任务结果存储形态，并保留同一字节中的错误与消费状态。
     * @param kind 当前存储形态。
     */
    void set_result_storage_kind(ResultStorageKind kind) noexcept
    {
        update_result_state(
            kResultStorageKindMask, static_cast<ResultStateBits>(kind));
    }

    /** @brief 判断任务是否通过错误通道完成。 */
    bool has_result_error() const noexcept
    {
        return (m_result_state.load(std::memory_order_acquire) &
                kResultErrorMask) != 0;
    }

    /** @brief 返回已记录的任务错误；仅应在 has_result_error() 为真时调用。 */
    detail::TaskResultError result_error() const noexcept
    {
        const auto encoded = static_cast<detail::TaskResultErrorCode>(
            ((m_result_state.load(std::memory_order_relaxed) &
              kResultErrorMask) >>
             kResultErrorShift) -
            1);
        return detail::TaskResultError(encoded);
    }

    /**
     * @brief 记录任务错误，并保留同一字节中的存储形态与消费状态。
     * @param error 待交付的任务错误。
     */
    void set_result_error(detail::TaskResultError error) noexcept
    {
        const auto encoded = static_cast<ResultStateBits>(
            1 + static_cast<ResultStateBits>(error.code()));
        update_result_state(kResultErrorMask, encoded << kResultErrorShift);
    }

    /**
     * @brief 原子地占有唯一一次结果消费资格。
     * @return 首次调用返回 true；已消费过返回 false。
     */
    bool try_mark_result_consumed() noexcept
    {
        return (m_result_state.fetch_or(kResultConsumedMask,
                                        std::memory_order_acq_rel) &
                kResultConsumedMask) == 0;
    }

    // Empty 状态下不读取结果字节；store() 先构造对象或写入指针，再发布存储类型。
    // 因此复用状态时无需清零尚未持有对象的 32 字节缓冲区。
    alignas(std::max_align_t) std::byte m_result_storage[kInlineResultBytes];  ///< 小对象内联结果存储
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
    // 结果错误、存储形态和单次消费标志共用一个原子字节：低两位保存存储
    // 形态，随后三位保存“错误码加一”，第五位表示结果已被消费。
    std::atomic<uint8_t> m_result_state{0};
    /**
     * @brief 销毁尚未消费的结果对象
     * @return 无返回值
     */
    void (*m_destroy_result)(TaskState&) noexcept = nullptr;
    TaskRef m_then;  ///< `then()` 追加的 continuation 任务
    TaskRef m_next;  ///< 当前 `co_await` 后要恢复的父任务

private:
    using ResultStateBits = uint8_t;
    static constexpr ResultStateBits kResultStorageKindMask = 0x03;
    static constexpr ResultStateBits kResultErrorShift = 2;
    static constexpr ResultStateBits kResultErrorMask = 0x1C;
    static constexpr ResultStateBits kResultConsumedMask = 0x20;
    static_assert(std::atomic<ResultStateBits>::is_always_lock_free);
    static_assert(static_cast<ResultStateBits>(
                      detail::TaskResultErrorCode::kResumeFailed) <
                  (kResultErrorMask >> kResultErrorShift));

    /**
     * @brief 原子替换结果状态中的指定位。
     * @param mask 待替换的位。
     * @param value 新位值。
     */
    void update_result_state(ResultStateBits mask,
                             ResultStateBits value) noexcept
    {
        // 错误和存储形态只会在完成发布前写入；完成后仅允许唯一消费方更新
        // 存储形态。消费标志本身通过 fetch_or 原子占位，因此这里保留其位即可。
        const ResultStateBits current =
            m_result_state.load(std::memory_order_relaxed);
        m_result_state.store(
            static_cast<ResultStateBits>((current & ~mask) | (value & mask)),
            std::memory_order_relaxed);
    }
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
    // 只有一个拥有引用时，不存在合法的并发 retain；acquire 接收其他引用
    // release 之前的写入，独占销毁无需再执行一次原子读改写。
    if (state->m_refs.load(std::memory_order_acquire) == 1 ||
        state->m_refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
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
        if (state->m_then.is_valid()) {
            if (auto* thenState = state->m_then.state(); thenState && thenState->m_runtime == nullptr) {
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
        if (state->m_then.is_valid() && state->m_then.belong_scheduler() == nullptr) {
            set_task_scheduler(state->m_then, scheduler);
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

struct TaskRefStorageAccess
{
    static TaskRef borrow(TaskState* state) noexcept
    {
        return TaskRef::borrowed(state);
    }

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

} // namespace detail

} // namespace galay::kernel

#endif // GALAY_KERNEL_TASK_REF_H
