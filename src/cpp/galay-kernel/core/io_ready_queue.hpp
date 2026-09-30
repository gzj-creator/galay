/**
 * @file io_ready_queue.hpp
 * @brief IO 就绪队列：本地 LIFO/ring、跨线程注入和恢复接纳。
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
#include <random>
#include <span>
#include <utility>
#include <vector>
#include <galay/thirdparty/concurrentqueue/moodycamel/concurrentqueue.h>

namespace galay::kernel {

/**
 * @brief 固定容量的 Chase-Lev 本地就绪环
 * @details 本地线程 push_back/pop_back，窃取者 steal_front。调用
 *          setStealingEnabled(false) 后（IO 调度器均如此）退化为 owner-only
 *          环：pop 不再发 seq_cst 仲裁栅栏，steal_front 直接拒绝。
 */
class ChaseLevTaskRing {
public:
    static constexpr size_t kCapacity = 256;
    static constexpr size_t kMask = kCapacity - 1;
    static_assert((kCapacity & (kCapacity - 1)) == 0,
                  "ChaseLevTaskRing capacity must be power of two");

    ChaseLevTaskRing() {
        for (auto& slot : m_slots) {
            slot.store(0, std::memory_order_relaxed);
        }
    }
    ~ChaseLevTaskRing() {
        clear();
    }
    ChaseLevTaskRing(const ChaseLevTaskRing&) = delete;
    ChaseLevTaskRing& operator=(const ChaseLevTaskRing&) = delete;

    bool push_back(detail::ReadyEntry& entry) {
        if (!entry.isValid()) {
            return false;
        }
        const uint64_t tail = m_tail.load(std::memory_order_relaxed);
        const uint64_t head = m_head.load(std::memory_order_acquire);
        if (tail - head >= kCapacity) {
            return false;
        }
        const size_t index = static_cast<size_t>(tail & kMask);
        const uintptr_t encoded = entry.encoded();
        if (!m_stealing_enabled) {
            // owner-only 路径：无窃取者竞争，直接 store 即可
            m_slots[index].store(encoded, std::memory_order_release);
        } else {
            uintptr_t expected = 0;
            if (!m_slots[index].compare_exchange_strong(expected, encoded,
                                                         std::memory_order_release,
                                                         std::memory_order_acquire)) {
                return false;
            }
        }
        entry.clear();
        m_tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    bool push_back(TaskRef&& task) {
        if (!task.isValid()) {
            return false;
        }
        detail::ReadyEntry entry(std::move(task));
        if (push_back(entry)) {
            return true;
        }
        task = detail::readyEntryToTaskRef(entry);
        return false;
    }

    bool pop_back(detail::ReadyEntry& out) {
        if (!m_stealing_enabled) {
            uint64_t tail = m_tail.load(std::memory_order_relaxed);
            const uint64_t head = m_head.load(std::memory_order_relaxed);
            // owner 独占游标，可先判空，避免空队列上的 tail 写入与回滚。
            if (tail <= head) {
                return false;
            }
            --tail;
            m_tail.store(tail, std::memory_order_relaxed);
            // 禁用 stealing 后只有 owner 能读写槽位，无需 exchange 的原子 RMW。
            // 仍清空槽位，保证停机后重新启用 stealing 时 CAS 能接纳新 entry。
            auto& slot = m_slots[static_cast<size_t>(tail & kMask)];
            const uintptr_t encoded = slot.load(std::memory_order_relaxed);
            slot.store(0, std::memory_order_relaxed);
            if (encoded == 0) {
                return false;
            }
            out = detail::ReadyEntry::fromEncoded(encoded);
            return true;
        }
        uint64_t tail = m_tail.load(std::memory_order_relaxed);
        if (tail == 0) {
            return false;
        }
        tail -= 1;
        m_tail.store(tail, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        uint64_t head = m_head.load(std::memory_order_relaxed);
        if (head > tail) {
            m_tail.store(head, std::memory_order_relaxed);
            return false;
        }
        const size_t index = static_cast<size_t>(tail & kMask);
        if (head == tail) {
            if (!m_head.compare_exchange_strong(head, tail + 1,
                                                std::memory_order_seq_cst,
                                                std::memory_order_relaxed)) {
                m_tail.store(head, std::memory_order_relaxed);
                return false;
            }
            m_tail.store(tail + 1, std::memory_order_relaxed);
            const uintptr_t encoded = m_slots[index].exchange(0, std::memory_order_relaxed);
            if (encoded == 0) {
                return false;
            }
            out = detail::ReadyEntry::fromEncoded(encoded);
            return true;
        }
        const uintptr_t encoded = m_slots[index].exchange(0, std::memory_order_relaxed);
        if (encoded == 0) {
            return false;
        }
        out = detail::ReadyEntry::fromEncoded(encoded);
        return true;
    }

    bool pop_back(TaskRef& out) {
        detail::ReadyEntry entry;
        if (!pop_back(entry)) {
            return false;
        }
        if (!entry.isCppTask()) {
            if (!push_back(entry)) {
                detail::releaseReadyEntry(entry);
            }
            return false;
        }
        out = detail::readyEntryToTaskRef(entry);
        return true;
    }

    bool steal_front(detail::ReadyEntry& out) {
        // 关闭 work-stealing 后本环只由 owner 线程访问；任何跨线程窃取都会
        // 破坏 pop_back 的 relaxed 快速路径，这里直接拒绝。
        if (!m_stealing_enabled) {
            return false;
        }
        uint64_t head = m_head.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        uint64_t tail = m_tail.load(std::memory_order_acquire);
        if (head >= tail) {
            return false;
        }
        const size_t index = static_cast<size_t>(head & kMask);
        if (!m_head.compare_exchange_strong(head, head + 1,
                                            std::memory_order_seq_cst,
                                            std::memory_order_relaxed)) {
            return false;
        }
        const uintptr_t encoded = m_slots[index].exchange(0, std::memory_order_relaxed);
        if (encoded == 0) {
            return false;
        }

        detail::ReadyEntry entry = detail::ReadyEntry::fromEncoded(encoded);
        if (detail::readyEntryResumeOwnerOnly(entry)) {
            out = std::move(entry);
            return false;
        }

        out = std::move(entry);
        return true;
    }

    bool steal_front(TaskRef& out) {
        detail::ReadyEntry entry;
        if (!steal_front(entry)) {
            if (entry.isValid() && !detail::scheduleReadyEntry(entry)) {
                detail::releaseReadyEntry(entry);
            }
            return false;
        }
        if (!entry.isCppTask()) {
            if (!detail::scheduleReadyEntry(entry)) {
                detail::releaseReadyEntry(entry);
            }
            return false;
        }
        out = detail::readyEntryToTaskRef(entry);
        return true;
    }

    // 仅在启动前或所有 owner/stealer 都已停止后切换模式。
    void setStealingEnabled(bool enabled) noexcept {
        m_stealing_enabled = enabled;
    }

    size_t size() const noexcept {
        const uint64_t head = m_head.load(std::memory_order_acquire);
        const uint64_t tail = m_tail.load(std::memory_order_acquire);
        return static_cast<size_t>(tail - head);
    }

    size_t remainingCapacity() const noexcept {
        const size_t current = size();
        return (current >= kCapacity) ? 0 : (kCapacity - current);
    }

    bool empty() const noexcept {
        return size() == 0;
    }

    void clear() noexcept {
        const uint64_t head = m_head.load(std::memory_order_relaxed);
        const uint64_t tail = m_tail.load(std::memory_order_relaxed);
        for (uint64_t index = head; index < tail; ++index) {
            const uintptr_t encoded =
                m_slots[index & kMask].exchange(0, std::memory_order_relaxed);
            if (encoded != 0) {
                detail::ReadyEntry entry = detail::ReadyEntry::fromEncoded(encoded);
                detail::releaseReadyEntry(entry);
            }
        }
        m_head.store(tail, std::memory_order_relaxed);
    }

private:
    std::array<std::atomic<uintptr_t>, kCapacity> m_slots{};
    std::atomic<uint64_t> m_head{0};
    std::atomic<uint64_t> m_tail{0};
    bool m_stealing_enabled = true;
};



/**
 * @brief IOScheduler work-stealing 计数快照
 * @details 计数器由 owner 线程独占写；读取方应在 runtime 停止后或外部同步下采样。
 */
struct IOSchedulerStealStats {
    uint64_t steal_attempts = 0;
    uint64_t steal_successes = 0;
};

/**
 * @brief IO 调度器执行线程的本地状态
 * @details 保存工作窃取缓冲、本地队列以及 LIFO 调度策略状态。
 * 该结构仅应在所属调度器线程内访问；`ready_inject_queue` 允许其他线程安全地注入 ready entry。
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
        clearPendingReadyEntries();
    }

    /**
     * @brief 调整跨线程注入批量缓冲区大小
     * @param inject_batch_size 目标批量大小；最小会被修正为 1
     */
    void resizeInjectBuffer(size_t inject_batch_size) {
        ready_inject_buffer.resize(std::max<size_t>(1, inject_batch_size));
    }

    /**
     * @brief 将任务推入本地执行队列
     * @param task 待入队的任务；无效任务会被忽略
     * @details 优先复用 LIFO 槽位以减少最近恢复任务的调度延迟
     */
    void scheduleLocal(TaskRef task) {
        if (!task.isValid()) {
            return;
        }
        scheduleLocal(detail::ReadyEntry(std::move(task)));
    }

    void scheduleLocal(detail::ReadyEntry entry) {
        if (!entry.isValid()) {
            return;
        }
        if (lifo_enabled) {
            if (ready_lifo_slot.has_value()) {
                detail::ReadyEntry deferred = std::move(*ready_lifo_slot);
                enqueueDeferred(deferred);
                ready_lifo_slot.reset();
            }
            ready_lifo_slot = std::move(entry);
            return;
        }
        enqueueDeferred(entry);
        detail::releaseReadyEntry(entry);
    }

    /**
     * @brief 将任务以 FIFO 方式追加到本地队列尾部
     * @param task 待入队的任务；无效任务会被忽略
     */
    void scheduleLocalDeferred(TaskRef task) {
        if (!task.isValid()) {
            return;
        }
        scheduleLocalDeferred(detail::ReadyEntry(std::move(task)));
    }

    void scheduleLocalDeferred(detail::ReadyEntry entry) {
        if (!entry.isValid()) {
            return;
        }
        enqueueDeferredFifo(entry);
        detail::releaseReadyEntry(entry);
    }

    /**
     * @brief 从其他线程安全地注入任务
     * @param task 待入队任务
     * @return 有值表示注入成功，值为 true 时注入前队列为空；std::nullopt 表示任务无效或入队失败
     */
    std::optional<bool> scheduleInjected(TaskRef task) {
        if (!task.isValid()) {
            return std::nullopt;
        }
        return scheduleInjected(detail::ReadyEntry(std::move(task)));
    }

    std::optional<bool> scheduleInjected(detail::ReadyEntry entry) {
        if (!entry.isValid()) {
            return std::nullopt;
        }
        const bool was_empty =
            injected_outstanding.fetch_add(1, std::memory_order_acq_rel) == 0;
        if (!ready_inject_queue.enqueue(std::move(entry))) {
            injected_outstanding.fetch_sub(1, std::memory_order_acq_rel);
            detail::releaseReadyEntry(entry);
            return std::nullopt;
        }
        return was_empty;
    }

    /**
     * @brief 无分配接纳已停泊 C++ 任务的恢复请求。
     * @return 有值表示接纳成功，值为 true 时接纳前没有远端待办；无效任务返回
     *         std::nullopt。
     * @details TaskState 自带侵入链接，因此该路径不依赖 ConcurrentQueue 分配。
     */
    std::optional<bool> scheduleResume(TaskRef task) noexcept {
        if (!task.isValid()) {
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

    /** @brief 停止接纳新的 owner-only 恢复请求；已接纳节点仍由 owner 排空。 */
    void closeResumeAdmission() noexcept {
        ready_resume_queue.close();
    }

    /**
     * @brief 在 scheduler 重启前重新开放恢复请求。
     * @return 前一运行周期已完整排空时返回 true；仍有 owner 本地节点时返回 false。
     */
    [[nodiscard]] bool reopenResumeAdmission() noexcept {
        return ready_resume_local == nullptr && ready_resume_queue.reopen();
    }

    /**
     * @brief 将跨线程注入队列中的任务搬运到本地队列
     * @return 实际拉取并转移到本地队列的任务数量
     */
    size_t drainInjected() {
        if (ready_inject_buffer.empty()) {
            return 0;
        }
        const size_t remaining = local_ring.remainingCapacity();
        if (remaining == 0) {
            return 0;
        }
        const bool resume_pending = hasPendingResume();
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
            owner_drained_injected_once.store(true, std::memory_order_release);
            injected_outstanding.fetch_sub(count, std::memory_order_acq_rel);
        }
        // ready_inject_queue 保持生产者顺序；逆序填充以使 owner 端 pop_back()
        // 仍按最旧优先的 FIFO 语义处理延后和溢出任务。
        for (size_t i = count; i > 0; --i) {
            detail::ReadyEntry entry = std::move(ready_inject_buffer[i - 1]);
            if (local_ring.push_back(entry)) {
                continue;
            }
            fallbackToInject(entry);
            detail::releaseReadyEntry(entry);
        }

        size_t resume_count = 0;
        if (count < remaining) {
            if (ready_resume_local == nullptr) {
                ready_resume_local = detail::TaskResumeQueue::reverse(
                    ready_resume_queue.takeAll());
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
                // local_ring 的 owner 端使用 pop_back()；反转本批节点后写入，
                // 才能让已经转成 FIFO 的 ready_resume_local 仍按 oldest-first 恢复。
                resume_batch = detail::TaskResumeQueue::reverse(resume_batch);
            }
            while (resume_batch != nullptr) {
                TaskRef task = detail::TaskResumeQueue::popFront(resume_batch);
                detail::ReadyEntry entry(std::move(task));
                if (!local_ring.push_back(entry)) {
                    TaskRef restored = detail::readyEntryToTaskRef(entry);
                    TaskState* state =
                        detail::TaskRefStorageAccess::releaseState(restored);
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
            owner_drained_injected_once.store(true, std::memory_order_release);
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
    bool hasPendingInjected() const {
        return injected_outstanding.load(std::memory_order_acquire) > 0;
    }

    /** @brief 是否仍有专用 resume 节点尚未搬入本地 ready ring。 */
    bool hasPendingResume() const noexcept {
        return ready_resume_local != nullptr || !ready_resume_queue.empty();
    }

    /**
     * @brief 判断 owner 线程是否已经至少处理过一批跨线程注入任务
     * @details stealing 只能旁路后续积压，不能抢走 victim 首次注入批次的 owner-first 执行机会。
     */
    bool hasOwnerDrainedInjected() const {
        return owner_drained_injected_once.load(std::memory_order_acquire);
    }

    /**
     * @brief 判断当前轮询周期是否应该检查注入队列
     * @return true 已达到检查阈值
     */
    bool shouldCheckInjected() const {
        return polls_since_inject >= inject_check_interval;
    }

    /**
     * @brief 判断本地执行队列是否仍有任务
     * @return true LIFO 槽位或 FIFO 队列中仍有待执行任务
     */
    bool hasLocalWork() const {
        return ready_lifo_slot.has_value() || !local_ring.empty();
    }

    /**
     * @brief 在取任务前整理本地调度状态
     * @details 当连续命中 LIFO 槽位过多时，将其回退到 FIFO 队列避免饥饿
     */
    void prepareForRun() {
        if (lifo_enabled && ready_lifo_slot.has_value() && consecutive_lifo_polls >= lifo_poll_limit) {
            detail::ReadyEntry deferred = std::move(*ready_lifo_slot);
            enqueueDeferred(deferred);
            detail::releaseReadyEntry(deferred);
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
    bool popNext(detail::ReadyEntry& out) {
        prepareForRun();

        if (ready_lifo_slot.has_value()) {
            out = std::move(*ready_lifo_slot);
            ready_lifo_slot.reset();
            ++consecutive_lifo_polls;
            ++polls_since_inject;
            return true;
        }

        if (local_ring.pop_back(out)) {
            lifo_enabled = true;
            consecutive_lifo_polls = 0;
            ++polls_since_inject;
            return true;
        }

        return false;
    }

    bool popNext(TaskRef& out) {
        detail::ReadyEntry entry;
        if (!popNext(entry)) {
            return false;
        }
        if (!entry.isCppTask()) {
            scheduleLocal(std::move(entry));
            return false;
        }
        out = detail::readyEntryToTaskRef(entry);
        return true;
    }

    /**
     * @brief 供 stealing 路径调用的入口
     */
    bool stealFront(detail::ReadyEntry& out) {
        if (local_ring.steal_front(out)) {
            return true;
        }
        if (out.isValid()) {
            fallbackToInject(out);
        }
        return false;
    }

    bool stealFront(TaskRef& out) {
        detail::ReadyEntry entry;
        if (!stealFront(entry)) {
            return false;
        }
        if (!entry.isCppTask()) {
            scheduleLocal(std::move(entry));
            return false;
        }
        out = detail::readyEntryToTaskRef(entry);
        return true;
    }

    /**
     * @brief 尝试从 sibling scheduler 偷任务（后续 task 需求）
     * @return true 表示 stealing 成功，应立即回到 ready pass
     */
    bool trySteal();

    IOSchedulerStealStats snapshotStealStats() const noexcept {
        return IOSchedulerStealStats{
            .steal_attempts = steal_attempts,
            .steal_successes = steal_successes,
        };
    }

    std::optional<detail::ReadyEntry> ready_lifo_slot;  ///< 实际使用的语言中立 LIFO ready 槽位
    ChaseLevTaskRing local_ring;        ///< 调度器线程本地固定容量 Chase-Lev ring
    moodycamel::ConcurrentQueue<detail::ReadyEntry> ready_inject_queue;  ///< 实际使用的语言中立跨线程注入队列
    std::vector<detail::ReadyEntry> ready_inject_buffer;  ///< ready_inject_queue 批量转移缓冲
    detail::TaskResumeQueue ready_resume_queue;  ///< Waker 专用无分配跨线程恢复队列
    TaskState* ready_resume_local = nullptr;  ///< owner 已摘取、尚未搬入本地 ring 的恢复链
    size_t self_index = 0;  ///< worker 在 steal-domain 中的位置
    std::span<IOScheduler* const> siblings;  ///< steal-domain 的只读 sibling 视图

    /**
     * @brief 配置本地 worker 的 steal-domain 元数据
     */
    void configureStealDomain(size_t index, std::span<IOScheduler* const> view) noexcept
    {
        self_index = index;
        siblings = view;
    }

    void setStealingEnabled(bool enabled) noexcept {
        stealing_enabled = enabled;
        local_ring.setStealingEnabled(enabled);
    }

    std::mt19937 random_seed{std::random_device{}()};  ///< 用于 victim 选择的随机器

    std::atomic<uint64_t> injected_outstanding{0};  ///< 尚未搬运到本地队列的注入任务数
    uint64_t steal_attempts = 0;  ///< trySteal() 进入真实 sibling 探测的次数
    uint64_t steal_successes = 0;  ///< trySteal() 成功窃取至少一个任务的次数
    uint32_t consecutive_lifo_polls = 0;  ///< 连续命中 ready_lifo_slot 的次数
    uint32_t lifo_poll_limit = 8;  ///< 允许连续走 LIFO 的最大次数
    uint32_t polls_since_inject = 0;  ///< 距离上次检查 ready_inject_queue 已轮询的任务数
    uint32_t inject_check_interval = 8;  ///< 检查 ready_inject_queue 的轮询间隔
    std::atomic<bool> owner_drained_injected_once{false};  ///< owner 线程是否已处理过注入队列
    bool lifo_enabled = true;  ///< 是否允许优先从 ready_lifo_slot 取任务
    bool stealing_enabled = true;  ///< 当前后端是否允许在 sibling 线程上恢复 stolen task
    bool prefer_resume_on_single_slot = true;  ///< 普通注入与 resume 同时积压时轮换最后一个 ring 槽

private:
    void clearPendingReadyEntries() noexcept {
        ready_lifo_slot.reset();
        local_ring.clear();

        detail::ReadyEntry entry;
        while (ready_inject_queue.try_dequeue(entry)) {
            detail::releaseReadyEntry(entry);
        }
        for (auto& buffered : ready_inject_buffer) {
            detail::releaseReadyEntry(buffered);
        }
        detail::TaskResumeQueue::releaseAll(ready_resume_local);
        ready_resume_local = nullptr;
        detail::TaskResumeQueue::releaseAll(ready_resume_queue.takeAll());
        injected_outstanding.store(0, std::memory_order_release);
    }

    void enqueueDeferred(detail::ReadyEntry& entry) {
        if (!entry.isValid()) {
            return;
        }
        if (local_ring.remainingCapacity() == 0) {
            fallbackToInject(entry);
            return;
        }
        if (!local_ring.push_back(entry)) {
            fallbackToInject(entry);
        }
    }

    void enqueueDeferredFifo(detail::ReadyEntry& entry) {
        if (!entry.isValid()) {
            return;
        }
        fallbackToInject(entry);
    }

    void fallbackToInject(detail::ReadyEntry& entry) {
        if (!entry.isValid()) {
            return;
        }
        injected_outstanding.fetch_add(1, std::memory_order_acq_rel);
        if (ready_inject_queue.enqueue(std::move(entry))) {
            return;
        }
        injected_outstanding.fetch_sub(1, std::memory_order_acq_rel);
        detail::releaseReadyEntry(entry);
    }
};

} // namespace galay::kernel
#endif
