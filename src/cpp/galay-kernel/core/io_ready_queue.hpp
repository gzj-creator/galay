/**
 * @file io_ready_queue.hpp
 * @brief IO 就绪队列：本地 LIFO 槽与就绪栈、跨线程注入和恢复接纳。
 * @note 本地消费仅限 owner 线程；跨线程生产通过注入和 resume 队列完成。
 */
#ifndef GALAY_KERNEL_IO_READY_QUEUE_HPP
#define GALAY_KERNEL_IO_READY_QUEUE_HPP

#include "scheduler.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>
#include <galay/thirdparty/concurrentqueue/moodycamel/concurrentqueue.h>

namespace galay::kernel {

/**
 * @brief owner 线程独占的固定容量本地就绪栈
 * @details 只由所属 IO 调度器线程读写，因此不使用原子操作；其他线程的提交
 *          经注入队列和 resume 队列转交 owner。pop_back 返回最近压入的条目，
 *          需要 FIFO 的批量搬运由调用方逆序压入。
 */
class LocalReadyStack {
public:
    static constexpr size_t kCapacity = 256;

    LocalReadyStack() noexcept = default;
    ~LocalReadyStack() {
        clear();
    }
    LocalReadyStack(const LocalReadyStack&) = delete;
    LocalReadyStack& operator=(const LocalReadyStack&) = delete;

    bool push_back(detail::ReadyEntry& entry) noexcept {
        if (!entry.is_valid() || m_size == kCapacity) {
            return false;
        }
        m_slots[m_size++] = entry.encoded();
        entry.clear();
        return true;
    }

    bool push_back(TaskRef&& task) {
        if (!task.is_valid()) {
            return false;
        }
        detail::ReadyEntry entry(std::move(task));
        if (push_back(entry)) {
            return true;
        }
        task = detail::ready_entry_to_task_ref(entry);
        return false;
    }

    bool pop_back(detail::ReadyEntry& out) noexcept {
        if (m_size == 0) {
            return false;
        }
        out = detail::ReadyEntry::from_encoded(m_slots[--m_size]);
        return true;
    }

    bool pop_back(TaskRef& out) {
        detail::ReadyEntry entry;
        if (!pop_back(entry)) {
            return false;
        }
        if (!entry.is_cpp_task()) {
            if (!push_back(entry)) {
                detail::release_ready_entry(entry);
            }
            return false;
        }
        out = detail::ready_entry_to_task_ref(entry);
        return true;
    }

    size_t size() const noexcept {
        return m_size;
    }

    size_t remaining_capacity() const noexcept {
        return kCapacity - m_size;
    }

    bool empty() const noexcept {
        return m_size == 0;
    }

    void clear() noexcept {
        while (m_size > 0) {
            detail::ReadyEntry entry = detail::ReadyEntry::from_encoded(m_slots[--m_size]);
            detail::release_ready_entry(entry);
        }
    }

private:
    std::array<uintptr_t, kCapacity> m_slots;  ///< 只有 [0, m_size) 持有有效条目
    size_t m_size = 0;
};

/**
 * @brief IO 调度器执行线程的本地状态
 * @details 保存本地就绪栈、跨线程注入缓冲以及 LIFO 调度策略状态。
 * 该结构仅应在所属调度器线程内访问；`ready_inject_queue` 允许其他线程安全地注入 ready entry。
 *          IO 注册和 reactor 提交都依赖这一 owner 线程亲和性，不能跨线程执行本地条目。
 */
struct IOReadyQueue {
    /**
     * @brief 构造工作线程局部队列状态
     * @param inject_batch_size 单次从跨线程注入队列中批量拉取的最大数量
     * @param lifo_limit 连续走 LIFO 槽位的最大次数，超过后回退到 FIFO
     * @param inject_interval 轮询多少次本地任务后检查一次注入队列
     */
    explicit IOReadyQueue(size_t inject_batch_size = GALAY_SCHEDULER_BATCH_SIZE,
                                    uint32_t lifo_limit = 8,
                                    uint32_t inject_interval = 8)
        : ready_inject_buffer(std::max<size_t>(1, inject_batch_size))
        , lifo_poll_limit(lifo_limit)
        , inject_check_interval(inject_interval)
    {
    }

    ~IOReadyQueue()
    {
        clear_pending_ready_entries();
    }

    /**
     * @brief 调整跨线程注入批量缓冲区大小
     * @param inject_batch_size 目标批量大小；最小会被修正为 1
     * @return 无返回值
     */
    void resize_inject_buffer(size_t inject_batch_size) {
        ready_inject_buffer.resize(std::max<size_t>(1, inject_batch_size));
    }

    /**
     * @brief 将任务推入本地执行队列
     * @param task 待入队的任务；无效任务会被忽略
     * @return 无返回值
     * @details 优先复用 LIFO 槽位以减少最近恢复任务的调度延迟
     */
    void schedule_local(TaskRef task) {
        if (!task.is_valid()) {
            return;
        }
        schedule_local(detail::ReadyEntry(std::move(task)));
    }

    void schedule_local(detail::ReadyEntry entry) {
        if (!entry.is_valid()) {
            return;
        }
        if (lifo_enabled) {
            if (ready_lifo_slot.has_value()) {
                detail::ReadyEntry deferred = std::move(*ready_lifo_slot);
                enqueue_deferred(deferred);
                ready_lifo_slot.reset();
            }
            ready_lifo_slot = std::move(entry);
            return;
        }
        enqueue_deferred(entry);
        detail::release_ready_entry(entry);
    }

    /**
     * @brief 将任务以 FIFO 方式追加到本地队列尾部
     * @param task 待入队的任务；无效任务会被忽略
     * @return 无返回值
     */
    void schedule_local_deferred(TaskRef task) {
        if (!task.is_valid()) {
            return;
        }
        schedule_local_deferred(detail::ReadyEntry(std::move(task)));
    }

    void schedule_local_deferred(detail::ReadyEntry entry) {
        if (!entry.is_valid()) {
            return;
        }
        enqueue_deferred_fifo(entry);
        detail::release_ready_entry(entry);
    }

    /**
     * @brief 从其他线程安全地注入任务
     * @param task 待入队任务
     * @return 有值表示注入成功，值为 true 时注入前队列为空；std::nullopt 表示任务无效或入队失败
     */
    std::optional<bool> schedule_injected(TaskRef task) {
        if (!task.is_valid()) {
            return std::nullopt;
        }
        return schedule_injected(detail::ReadyEntry(std::move(task)));
    }

    std::optional<bool> schedule_injected(detail::ReadyEntry entry) {
        if (!entry.is_valid()) {
            return std::nullopt;
        }
        const bool was_empty =
            injected_outstanding.fetch_add(1, std::memory_order_acq_rel) == 0;
        if (!ready_inject_queue.enqueue(std::move(entry))) {
            injected_outstanding.fetch_sub(1, std::memory_order_acq_rel);
            detail::release_ready_entry(entry);
            return std::nullopt;
        }
        return was_empty;
    }

    /**
     * @brief 无分配接纳已停泊 C++ 任务的恢复请求。
     * @param task 协程任务
     * @return 有值表示接纳成功，值为 true 时接纳前没有远端待办；无效任务返回
     *         std::nullopt。
     * @details TaskState 自带侵入链接，因此该路径不依赖 ConcurrentQueue 分配。
     */
    std::optional<bool> schedule_resume(TaskRef task) noexcept {
        if (!task.is_valid()) {
            return std::nullopt;
        }
        const bool was_empty =
            injected_outstanding.fetch_add(1, std::memory_order_acq_rel) == 0;
        if (!ready_resume_queue.push(std::move(task))) {
            injected_outstanding.fetch_sub(1, std::memory_order_acq_rel);
            return std::nullopt;
        }
        return was_empty;
    }

    /**
     * @brief 停止接纳新的 owner-only 恢复请求；已接纳节点仍由 owner 排空。
     * @return 无返回值
     */
    void close_resume_admission() noexcept {
        ready_resume_queue.close();
    }

    /**
     * @brief 在 scheduler 重启前重新开放恢复请求。
     * @return 前一运行周期已完整排空时返回 true；仍有 owner 本地节点时返回 false。
     */
    [[nodiscard]] bool reopen_resume_admission() noexcept {
        return ready_resume_local == nullptr && ready_resume_queue.reopen();
    }

    /**
     * @brief 将跨线程注入队列中的任务搬运到本地队列
     * @return 实际拉取并转移到本地队列的任务数量
     */
    size_t drain_injected() {
        if (ready_inject_buffer.empty()) {
            return 0;
        }
        const size_t remaining = local_stack.remaining_capacity();
        if (remaining == 0) {
            return 0;
        }
        const bool resume_pending = has_pending_resume();
        size_t normal_capacity = remaining;
        if (resume_pending) {
            if (remaining > 1) {
                normal_capacity = remaining - 1;
            } else if (prefer_resume_on_single_slot) {
                normal_capacity = 0;
            }
        }
        const size_t target =
            std::min(normal_capacity, ready_inject_buffer.size());
        const size_t count = ready_inject_queue.try_dequeue_bulk(ready_inject_buffer.data(), target);
        if (count > 0) {
            injected_outstanding.fetch_sub(count, std::memory_order_acq_rel);
        }
        // ready_inject_queue 保持生产者顺序；逆序填充以使 owner 端 pop_back()
        // 仍按最旧优先的 FIFO 语义处理延后和溢出任务。
        for (size_t i = count; i > 0; --i) {
            detail::ReadyEntry entry = std::move(ready_inject_buffer[i - 1]);
            if (local_stack.push_back(entry)) {
                continue;
            }
            fallback_to_inject(entry);
            detail::release_ready_entry(entry);
        }

        size_t resume_count = 0;
        if (count < remaining) {
            if (ready_resume_local == nullptr) {
                ready_resume_local = detail::TaskResumeQueue::reverse(
                    ready_resume_queue.take_all());
            }
            const size_t resume_limit = remaining - count;
            TaskState* resume_batch = ready_resume_local;
            TaskState* resume_batch_tail = nullptr;
            size_t selected = 0;
            while (ready_resume_local != nullptr && selected < resume_limit) {
                resume_batch_tail = ready_resume_local;
                ready_resume_local = ready_resume_local->m_resume_queue_next;
                ++selected;
            }
            if (resume_batch_tail != nullptr) {
                resume_batch_tail->m_resume_queue_next = nullptr;
                // local_stack 的 owner 端使用 pop_back()；反转本批节点后写入，
                // 才能让已经转成 FIFO 的 ready_resume_local 仍按 oldest-first 恢复。
                resume_batch = detail::TaskResumeQueue::reverse(resume_batch);
            }
            while (resume_batch != nullptr) {
                TaskRef task = detail::TaskResumeQueue::pop_front(resume_batch);
                detail::ReadyEntry entry(std::move(task));
                if (!local_stack.push_back(entry)) {
                    TaskRef restored = detail::ready_entry_to_task_ref(entry);
                    TaskState* state =
                        detail::TaskRefStorageAccess::release_state(restored);
                    state->m_resume_queue_next = resume_batch;
                    TaskState* restored_fifo =
                        detail::TaskResumeQueue::reverse(state);
                    TaskState* restored_tail = restored_fifo;
                    while (restored_tail->m_resume_queue_next != nullptr) {
                        restored_tail = restored_tail->m_resume_queue_next;
                    }
                    restored_tail->m_resume_queue_next = ready_resume_local;
                    ready_resume_local = restored_fifo;
                    break;
                }
                ++resume_count;
            }
        }
        if (resume_count > 0) {
            injected_outstanding.fetch_sub(resume_count, std::memory_order_acq_rel);
        }
        if (remaining == 1 && resume_pending) {
            if (resume_count > 0) {
                prefer_resume_on_single_slot = false;
            } else if (count > 0) {
                prefer_resume_on_single_slot = true;
            }
        }
        polls_since_inject = 0;
        return count + resume_count;
    }

    /**
     * @brief 判断是否仍有跨线程注入任务待处理
     * @return true 仍有任务未从注入队列转移到本地队列
     */
    bool has_pending_injected() const {
        return injected_outstanding.load(std::memory_order_acquire) > 0;
    }

    /**
     * @brief 是否仍有专用 resume 节点尚未搬入本地就绪栈。
     * @return 满足所检查条件时返回 true，否则返回 false
     */
    bool has_pending_resume() const noexcept {
        return ready_resume_local != nullptr || !ready_resume_queue.empty();
    }

    /**
     * @brief 判断当前轮询周期是否应该检查注入队列
     * @return true 已达到检查阈值
     */
    bool should_check_injected() const {
        return polls_since_inject >= inject_check_interval;
    }

    /**
     * @brief 判断本地执行队列是否仍有任务
     * @return true LIFO 槽位或 FIFO 队列中仍有待执行任务
     */
    bool has_local_work() const {
        return ready_lifo_slot.has_value() || !local_stack.empty();
    }

    /**
     * @brief 在取任务前整理本地调度状态
     * @return 无返回值
     * @details 当连续命中 LIFO 槽位过多时，将其回退到 FIFO 队列避免饥饿
     */
    void prepare_for_run() {
        if (lifo_enabled && ready_lifo_slot.has_value() && consecutive_lifo_polls >= lifo_poll_limit) {
            detail::ReadyEntry deferred = std::move(*ready_lifo_slot);
            enqueue_deferred(deferred);
            detail::release_ready_entry(deferred);
            ready_lifo_slot.reset();
            lifo_enabled = false;
            consecutive_lifo_polls = 0;
        }
    }

    /**
     * @brief 从本地状态中取出下一条待执行任务
     * @param out 成功时写入取出的任务
     * @return true 取到了任务；false 本地无任务可执行
     */
    bool pop_next(detail::ReadyEntry& out) {
        prepare_for_run();

        if (ready_lifo_slot.has_value()) {
            out = std::move(*ready_lifo_slot);
            ready_lifo_slot.reset();
            ++consecutive_lifo_polls;
            ++polls_since_inject;
            return true;
        }

        if (local_stack.pop_back(out)) {
            lifo_enabled = true;
            consecutive_lifo_polls = 0;
            ++polls_since_inject;
            return true;
        }

        return false;
    }

    bool pop_next(TaskRef& out) {
        detail::ReadyEntry entry;
        if (!pop_next(entry)) {
            return false;
        }
        if (!entry.is_cpp_task()) {
            schedule_local(std::move(entry));
            return false;
        }
        out = detail::ready_entry_to_task_ref(entry);
        return true;
    }

    std::optional<detail::ReadyEntry> ready_lifo_slot;  ///< 实际使用的语言中立 LIFO ready 槽位
    LocalReadyStack local_stack;        ///< owner 线程独占的固定容量本地就绪栈
    moodycamel::ConcurrentQueue<detail::ReadyEntry> ready_inject_queue;  ///< 实际使用的语言中立跨线程注入队列
    std::vector<detail::ReadyEntry> ready_inject_buffer;  ///< ready_inject_queue 批量转移缓冲
    detail::TaskResumeQueue ready_resume_queue;  ///< Waker 专用无分配跨线程恢复队列
    TaskState* ready_resume_local = nullptr;  ///< owner 已摘取、尚未搬入本地栈的恢复链

    std::atomic<uint64_t> injected_outstanding{0};  ///< 尚未搬运到本地队列的注入任务数
    uint32_t consecutive_lifo_polls = 0;  ///< 连续命中 ready_lifo_slot 的次数
    uint32_t lifo_poll_limit = 8;  ///< 允许连续走 LIFO 的最大次数
    uint32_t polls_since_inject = 0;  ///< 距离上次检查 ready_inject_queue 已轮询的任务数
    uint32_t inject_check_interval = 8;  ///< 检查 ready_inject_queue 的轮询间隔
    bool lifo_enabled = true;  ///< 是否允许优先从 ready_lifo_slot 取任务
    bool prefer_resume_on_single_slot = true;  ///< 普通注入与 resume 同时积压时轮换最后一个本地槽

private:
    void clear_pending_ready_entries() noexcept {
        ready_lifo_slot.reset();
        local_stack.clear();

        detail::ReadyEntry entry;
        while (ready_inject_queue.try_dequeue(entry)) {
            detail::release_ready_entry(entry);
        }
        for (auto& buffered : ready_inject_buffer) {
            detail::release_ready_entry(buffered);
        }
        detail::TaskResumeQueue::release_all(ready_resume_local);
        ready_resume_local = nullptr;
        detail::TaskResumeQueue::release_all(ready_resume_queue.take_all());
        injected_outstanding.store(0, std::memory_order_release);
    }

    void enqueue_deferred(detail::ReadyEntry& entry) {
        if (!entry.is_valid()) {
            return;
        }
        if (local_stack.remaining_capacity() == 0) {
            fallback_to_inject(entry);
            return;
        }
        if (!local_stack.push_back(entry)) {
            fallback_to_inject(entry);
        }
    }

    void enqueue_deferred_fifo(detail::ReadyEntry& entry) {
        if (!entry.is_valid()) {
            return;
        }
        fallback_to_inject(entry);
    }

    void fallback_to_inject(detail::ReadyEntry& entry) {
        if (!entry.is_valid()) {
            return;
        }
        injected_outstanding.fetch_add(1, std::memory_order_acq_rel);
        if (ready_inject_queue.enqueue(std::move(entry))) {
            return;
        }
        injected_outstanding.fetch_sub(1, std::memory_order_acq_rel);
        detail::release_ready_entry(entry);
    }
};

} // namespace galay::kernel
#endif
