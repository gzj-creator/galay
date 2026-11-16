/**
 * @file ready_entry.h
 * @brief 调度器 ready queue 的内部条目与恢复队列
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 定义 TaskResumeQueue、ReadyEntry 和恢复入口。该头仅供调度器及其
 *          测试使用，普通 Task 消费者只需包含 task_ref.h 或 task.h。
 */

#ifndef GALAY_KERNEL_READY_ENTRY_H
#define GALAY_KERNEL_READY_ENTRY_H

#include "task_ref.h"

#include <atomic>
#include <cstdint>
#include <utility>

namespace galay::kernel
{

namespace detail
{

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
 *          void* state 查询 owner、恢复入口和 ready 引用释放入口。
 */
struct ReadyEntryHooks
{
    /**
     * @brief 返回 owner scheduler
     * @param state 状态对象
     * @return Scheduler* 指针
     */
    Scheduler* (*owner_scheduler)(void* state) noexcept = nullptr;
    /**
     * @brief 恢复 C 协程；成功后 ready entry 会释放
     * @param state 状态对象
     * @return 操作成功时返回 true，否则返回 false
     */
    bool (*resume)(void* state) noexcept = nullptr;
    /**
     * @brief 释放 ready entry 持有的 C 协程引用
     * @param state 状态对象
     * @return 无返回值
     */
    void (*release)(void* state) noexcept = nullptr;
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

inline bool resume_task_state(TaskState* state)
{
    if (!state || !state->m_handle || state->is_done(std::memory_order_relaxed)) {
        return false;
    }
    state->m_queued.store(false, std::memory_order_relaxed);
    state->m_resume_queue_claimed.store(false, std::memory_order_release);
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

} // namespace galay::kernel

#endif // GALAY_KERNEL_READY_ENTRY_H
