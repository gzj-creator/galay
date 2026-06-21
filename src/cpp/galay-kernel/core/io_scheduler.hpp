/**
 * @file io_scheduler.hpp
 * @brief IO 调度器接口、工作线程状态及 Chase-Lev 工作窃取环
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 定义：
 * - IOScheduler：具体后端（epoll、kqueue、io_uring）实现的抽象基类
 * - IOSchedulerWorkerState：每线程本地队列、LIFO 槽位和跨线程注入队列
 * - ChaseLevTaskRing：固定容量无锁环，支持 owner push / steal-front 语义
 * - IOController::getAwaitable<T> 特化集合，提供类型安全的 awaitable 访问
 * - completeAwaitableAndWake：通用 IO 完成 + 协程唤醒辅助函数
 */

#ifndef GALAY_KERNEL_IOSCHEDULER_HPP
#define GALAY_KERNEL_IOSCHEDULER_HPP

#include "../common/defn.hpp"
#include "../common/error.h"
#include "scheduler.hpp"
#include "io_controller.hpp"
#include "awaitable.h"
#include "../common/timer_manager.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <optional>
#include <random>
#include <span>
#include <utility>
#include <vector>
#include <concurrentqueue/moodycamel/concurrentqueue.h>

namespace galay::kernel
{

/**
 * @def GALAY_KERNEL_IO_POLL_WAIT_MAX_NS
 * @brief 与 TimerManager tick 对齐的 poll 阻塞上限（纳秒），与 io_uring 默认等待一致
 */
#ifndef GALAY_KERNEL_IO_POLL_WAIT_MAX_NS
#define GALAY_KERNEL_IO_POLL_WAIT_MAX_NS 10000000ULL
#endif

namespace detail {

/**
 * @brief 由时间轮 tick 推导半 tick 的 poll 等待（纳秒），带下限与上限，供 kqueue/io_uring 使用
 */
inline uint64_t halfTickPollWaitNanoseconds(uint64_t tick_duration_ns) noexcept {
    uint64_t half = tick_duration_ns / 2;
    constexpr uint64_t kZeroFallbackNs = 1000000ULL;
    if (half == 0) {
        half = kZeroFallbackNs;
    }
    if (half > GALAY_KERNEL_IO_POLL_WAIT_MAX_NS) {
        half = GALAY_KERNEL_IO_POLL_WAIT_MAX_NS;
    }
    return half;
}

/**
 * @brief 由时间轮 tick 推导 epoll_wait 超时毫秒数（半 tick，至少 1ms）
 */
inline int halfTickPollTimeoutMilliseconds(uint64_t tick_duration_ns) noexcept {
    int ms = static_cast<int>(tick_duration_ns / 2000000ULL);
    return ms < 1 ? 1 : ms;
}

/**
 * @brief 填充 kevent 使用的 timespec，与 `halfTickPollWaitNanoseconds` 语义一致
 */
inline void fillTimespecHalfTick(struct ::timespec& ts, uint64_t tick_duration_ns) noexcept {
    const uint64_t ns = halfTickPollWaitNanoseconds(tick_duration_ns);
    ts.tv_sec = static_cast<::time_t>(ns / 1000000000ULL);
    ts.tv_nsec = static_cast<long>(ns % 1000000000ULL);
}

}  // namespace detail

/**
 * @brief 固定容量的 Chase-Lev 本地就绪环
 * @details 只服务于 IOSchedulerWorkState，本地线程 push_back/pop_back，窃取者 steal_front。
 */
class ChaseLevTaskRing {
public:
    static constexpr size_t kCapacity = 256;
    static constexpr size_t kMask = kCapacity - 1;
    static_assert((kCapacity & (kCapacity - 1)) == 0,
                  "ChaseLevTaskRing capacity must be power of two");

    ChaseLevTaskRing() {
        for (auto& slot : m_slots) {
            slot.store(nullptr, std::memory_order_relaxed);
        }
    }
    ~ChaseLevTaskRing() {
        clear();
    }
    ChaseLevTaskRing(const ChaseLevTaskRing&) = delete;
    ChaseLevTaskRing& operator=(const ChaseLevTaskRing&) = delete;

    bool push_back(TaskRef&& task) {
        if (!task.isValid()) {
            return false;
        }
        const uint64_t tail = m_tail.load(std::memory_order_relaxed);
        const uint64_t head = m_head.load(std::memory_order_acquire);
        if (tail - head >= kCapacity) {
            return false;
        }
        const size_t index = static_cast<size_t>(tail & kMask);
        TaskState* expected = nullptr;
        TaskState* const state = task.state();
        if (!m_slots[index].compare_exchange_strong(expected, state,
                                                     std::memory_order_release,
                                                     std::memory_order_acquire)) {
            return false;
        }
        (void)detail::TaskRefStorageAccess::releaseState(task);
        std::atomic_thread_fence(std::memory_order_release);
        m_tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    bool pop_back(TaskRef& out) {
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
            TaskState* const state = m_slots[index].exchange(nullptr, std::memory_order_relaxed);
            if (state == nullptr) {
                return false;
            }
            out = detail::TaskRefStorageAccess::adoptState(state);
            return true;
        }
        TaskState* const state = m_slots[index].exchange(nullptr, std::memory_order_relaxed);
        if (state == nullptr) {
            return false;
        }
        out = detail::TaskRefStorageAccess::adoptState(state);
        return true;
    }

    bool steal_front(TaskRef& out) {
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
        TaskState* const state = m_slots[index].exchange(nullptr, std::memory_order_relaxed);
        if (state == nullptr) {
            return false;
        }

        TaskRef task = detail::TaskRefStorageAccess::adoptState(state);
        if (state->m_resume_owner_only.load(std::memory_order_acquire)) {
            if (auto* scheduler = task.belongScheduler()) {
                scheduler->schedule(std::move(task));
            }
            return false;
        }

        out = std::move(task);
        return true;
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
            TaskState* const state =
                m_slots[index & kMask].exchange(nullptr, std::memory_order_relaxed);
            if (state != nullptr) {
                [[maybe_unused]] TaskRef released =
                    detail::TaskRefStorageAccess::adoptState(state);
            }
        }
        m_head.store(tail, std::memory_order_relaxed);
    }

private:
    std::array<std::atomic<TaskState*>, kCapacity> m_slots{};
    std::atomic<uint64_t> m_head{0};
    std::atomic<uint64_t> m_tail{0};
};

class IOScheduler;

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
 * 该结构仅应在所属调度器线程内访问；`inject_queue` 允许其他线程安全地注入任务。
 */
struct IOSchedulerWorkerState {
    /**
     * @brief 构造工作线程局部队列状态
     * @param inject_batch_size 单次从跨线程注入队列中批量拉取的最大数量
     * @param lifo_limit 连续走 LIFO 槽位的最大次数，超过后回退到 FIFO
     * @param inject_interval 轮询多少次本地任务后检查一次注入队列
     */
    explicit IOSchedulerWorkerState(size_t inject_batch_size = GALAY_SCHEDULER_BATCH_SIZE,
                                    uint32_t lifo_limit = 8,
                                    uint32_t inject_interval = 8)
        : inject_buffer(std::max<size_t>(1, inject_batch_size))
        , lifo_poll_limit(lifo_limit)
        , inject_check_interval(inject_interval)
    {
    }

    /**
     * @brief 调整跨线程注入批量缓冲区大小
     * @param inject_batch_size 目标批量大小；最小会被修正为 1
     */
    void resizeInjectBuffer(size_t inject_batch_size) {
        inject_buffer.resize(std::max<size_t>(1, inject_batch_size));
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
        if (lifo_enabled) {
            if (lifo_slot.has_value()) {
                enqueueDeferred(std::move(*lifo_slot));
            }
            lifo_slot = std::move(task);
            return;
        }
        enqueueDeferred(std::move(task));
    }

    /**
     * @brief 将任务以 FIFO 方式追加到本地队列尾部
     * @param task 待入队的任务；无效任务会被忽略
     */
    void scheduleLocalDeferred(TaskRef task) {
        if (!task.isValid()) {
            return;
        }
        enqueueDeferredFifo(std::move(task));
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
        const bool was_empty =
            injected_outstanding.fetch_add(1, std::memory_order_acq_rel) == 0;
        if (!inject_queue.enqueue(std::move(task))) {
            injected_outstanding.fetch_sub(1, std::memory_order_acq_rel);
            return std::nullopt;
        }
        return was_empty;
    }

    /**
     * @brief 将跨线程注入队列中的任务搬运到本地队列
     * @return 实际拉取并转移到本地队列的任务数量
     */
    size_t drainInjected() {
        if (inject_buffer.empty()) {
            return 0;
        }
        const size_t remaining = local_ring.remainingCapacity();
        if (remaining == 0) {
            return 0;
        }
        const size_t target = std::min(remaining, inject_buffer.size());
        const size_t count = inject_queue.try_dequeue_bulk(inject_buffer.data(), target);
        if (count > 0) {
            owner_drained_injected_once.store(true, std::memory_order_release);
            injected_outstanding.fetch_sub(count, std::memory_order_acq_rel);
        }
        // inject_queue 保持生产者顺序；逆序填充以使 owner 端 pop_back()
        // 仍按最旧优先的 FIFO 语义处理延后和溢出任务。
        for (size_t i = count; i > 0; --i) {
            TaskRef task = std::move(inject_buffer[i - 1]);
            if (local_ring.push_back(std::move(task))) {
                continue;
            }
            fallbackToInject(std::move(task));
        }
        polls_since_inject = 0;
        return count;
    }

    /**
     * @brief 判断是否仍有跨线程注入任务待处理
     * @return true 仍有任务未从注入队列转移到本地队列
     */
    bool hasPendingInjected() const {
        return injected_outstanding.load(std::memory_order_acquire) > 0;
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
        return lifo_slot.has_value() || !local_ring.empty();
    }

    /**
     * @brief 在取任务前整理本地调度状态
     * @details 当连续命中 LIFO 槽位过多时，将其回退到 FIFO 队列避免饥饿
     */
    void prepareForRun() {
        if (lifo_enabled && lifo_slot.has_value() && consecutive_lifo_polls >= lifo_poll_limit) {
            enqueueDeferred(std::move(*lifo_slot));
            lifo_slot.reset();
            lifo_enabled = false;
            consecutive_lifo_polls = 0;
        }
    }

    /**
     * @brief 从本地状态中取出下一条待执行任务
     * @param out 成功时写入取出的任务
     * @return true 取到了任务；false 本地无任务可执行
     */
    bool popNext(TaskRef& out) {
        prepareForRun();

        if (lifo_slot.has_value()) {
            out = std::move(*lifo_slot);
            lifo_slot.reset();
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

    /**
     * @brief 供 stealing 路径调用的入口
     */
    bool stealFront(TaskRef& out) {
        return local_ring.steal_front(out);
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

    std::optional<TaskRef> lifo_slot;  ///< 最近一次入队的任务，优先以内联 LIFO 方式调度
    ChaseLevTaskRing local_ring;        ///< 调度器线程本地固定容量 Chase-Lev ring
    moodycamel::ConcurrentQueue<TaskRef> inject_queue;  ///< 其他线程注入任务的无锁队列
    std::vector<TaskRef> inject_buffer;  ///< 从 inject_queue 批量转移任务时复用的临时缓冲
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
    }

    std::mt19937 random_seed{std::random_device{}()};  ///< 用于 victim 选择的随机器

    std::atomic<uint64_t> injected_outstanding{0};  ///< 尚未搬运到本地队列的注入任务数
    std::atomic<bool> owner_drained_injected_once{false};  ///< owner 线程是否已处理过注入队列
    uint32_t consecutive_lifo_polls = 0;  ///< 连续命中 lifo_slot 的次数
    uint32_t lifo_poll_limit = 8;  ///< 允许连续走 LIFO 的最大次数
    uint32_t polls_since_inject = 0;  ///< 距离上次检查 inject_queue 已轮询的任务数
    uint32_t inject_check_interval = 8;  ///< 检查 inject_queue 的轮询间隔
    bool lifo_enabled = true;  ///< 是否允许优先从 lifo_slot 取任务
    bool stealing_enabled = true;  ///< 当前后端是否允许在 sibling 线程上恢复 stolen task
    uint64_t steal_attempts = 0;  ///< trySteal() 进入真实 sibling 探测的次数
    uint64_t steal_successes = 0;  ///< trySteal() 成功窃取至少一个任务的次数

private:
    void enqueueDeferred(TaskRef task) {
        if (!task.isValid()) {
            return;
        }
        if (local_ring.remainingCapacity() == 0) {
            fallbackToInject(std::move(task));
            return;
        }
        if (!local_ring.push_back(std::move(task))) {
            fallbackToInject(std::move(task));
        }
    }

    void enqueueDeferredFifo(TaskRef task) {
        if (!task.isValid()) {
            return;
        }
        injected_outstanding.fetch_add(1, std::memory_order_acq_rel);
        inject_queue.enqueue(std::move(task));
    }

    void fallbackToInject(TaskRef task) {
        if (!task.isValid()) {
            return;
        }
        injected_outstanding.fetch_add(1, std::memory_order_acq_rel);
        inject_queue.enqueue(std::move(task));
    }
};

/**
 * @brief IOController::getAwaitable 的显式特化集合
 * @details 这些访问器把 READ/WRITE 槽位上的 `void*` awaitable 安全转换为具体类型。
 */

template<>
inline auto IOController::getAwaitable() -> AcceptAwaitable* {
    return static_cast<AcceptAwaitable*>(m_awaitable[READ]);
}

template<>
inline auto IOController::getAwaitable() -> RecvAwaitable* {
    return static_cast<RecvAwaitable*>(m_awaitable[READ]);
}

template<>
inline auto IOController::getAwaitable() -> SendAwaitable* {
    return static_cast<SendAwaitable*>(m_awaitable[WRITE]);
}

template<>
inline auto IOController::getAwaitable() -> ConnectAwaitable* {
    return static_cast<ConnectAwaitable*>(m_awaitable[WRITE]);
}

template<>
inline auto IOController::getAwaitable() -> RecvFromAwaitable* {
    return static_cast<RecvFromAwaitable*>(m_awaitable[READ]);
}

template<>
inline auto IOController::getAwaitable() -> SendToAwaitable* {
    return static_cast<SendToAwaitable*>(m_awaitable[WRITE]);
}

template<>
inline auto IOController::getAwaitable() -> FileReadAwaitable* {
    return static_cast<FileReadAwaitable*>(m_awaitable[READ]);
}

template<>
inline auto IOController::getAwaitable() -> FileWriteAwaitable* {
    return static_cast<FileWriteAwaitable*>(m_awaitable[WRITE]);
}

template<>
inline auto IOController::getAwaitable() -> FileWatchAwaitable* {
    return static_cast<FileWatchAwaitable*>(m_awaitable[READ]);
}

template<>
inline auto IOController::getAwaitable() -> ReadvAwaitable* {
    return static_cast<ReadvAwaitable*>(m_awaitable[READ]);
}

template<>
inline auto IOController::getAwaitable() -> WritevAwaitable* {
    return static_cast<WritevAwaitable*>(m_awaitable[WRITE]);
}

template<>
inline auto IOController::getAwaitable() -> SendFileAwaitable* {
    return static_cast<SendFileAwaitable*>(m_awaitable[WRITE]);
}

/**
 * @brief 完成 awaitable 并在需要时唤醒关联协程
 * @tparam Awaitable 具体 awaitable 类型
 * @param controller 触发完成的 IO 控制器
 * @param awaitable 与该 IO 完成事件关联的 awaitable
 * @note 仅当 `handleComplete()` 返回 true 时才真正唤醒协程
 */
template<typename Awaitable>
inline void completeAwaitableAndWake(IOController* controller, Awaitable* awaitable) {
    if (awaitable && awaitable->handleComplete(controller->m_handle)) {
        awaitable->m_waker.wakeUp();
    }
}

/**
 * @brief IO调度器接口
 *
 * @details 扩展Scheduler，添加IO事件注册接口。
 * 具体实现需要处理平台相关的IO多路复用机制。
 * 接口全都是非线程安全的，只能在调度器内部使用或绑定当前调度器的协程使用
 *
 * @note
 * - macOS: KqueueScheduler (kqueue)
 * - Linux: EpollScheduler (epoll) 或 IOUringScheduler (io_uring)
 *
 * @see KqueueScheduler
 */
class IOScheduler: public Scheduler
{
public:
    /**
     * @brief 返回调度器类型
     * @return 固定返回 kIOScheduler
     */
    SchedulerType type() override {
        return kIOScheduler;
    }

    /**
     * @brief 注册Accept事件
     * @param controller IO控制器
     * @return 1表示立即完成，0表示已注册等待，<0表示错误
     */
    virtual int addAccept(IOController* controller) = 0;

    /**
     * @brief 注册Connect事件
     * @param controller IO控制器
     * @return 1表示立即完成，0表示已注册等待，<0表示错误
     */
    virtual int addConnect(IOController* controller) = 0;

    /**
     * @brief 注册Recv事件
     * @param controller IO控制器
     * @return 1表示立即完成，0表示已注册等待，<0表示错误
     */
    virtual int addRecv(IOController* controller) = 0;

    /**
     * @brief 注册Send事件
     * @param controller IO控制器
     * @return 1表示立即完成，0表示已注册等待，<0表示错误
     */
    virtual int addSend(IOController* controller) = 0;

    /**
     * @brief 注册Readv事件（scatter-gather 读取）
     * @param controller IO控制器
     * @return 1表示立即完成，0表示已注册等待，<0表示错误
     */
    virtual int addReadv(IOController* controller) = 0;

    /**
     * @brief 注册Writev事件（scatter-gather 写入）
     * @param controller IO控制器
     * @return 1表示立即完成，0表示已注册等待，<0表示错误
     */
    virtual int addWritev(IOController* controller) = 0;

    /**
     * @brief 关闭文件描述符
     * @param controller IO控制器
     * @return 0表示成功，<0表示错误
     */
    virtual int addClose(IOController* contoller) = 0;

    /**
     * @brief 注册文件读取事件
     * @param controller IO控制器
     * @return 1表示立即完成，0表示已注册等待，<0表示错误
     */
    virtual int addFileRead(IOController* controller) = 0;

    /**
     * @brief 注册文件写入事件
     * @param controller IO控制器
     * @return 1表示立即完成，0表示已注册等待，<0表示错误
     */
    virtual int addFileWrite(IOController* controller) = 0;

    /**
     * @brief 注册RecvFrom事件（UDP接收）
     * @param controller IO控制器
     * @return 1表示立即完成，0表示已注册等待，<0表示错误
     */
    virtual int addRecvFrom(IOController* controller) = 0;

    /**
     * @brief 注册SendTo事件（UDP发送）
     * @param controller IO控制器
     * @return 1表示立即完成，0表示已注册等待，<0表示错误
     */
    virtual int addSendTo(IOController* controller) = 0;

    /**
     * @brief 注册文件监控事件
     * @param controller IO控制器
     * @return 1表示立即完成，0表示已注册等待，<0表示错误
     */
    virtual int addFileWatch(IOController* controller) = 0;

    /**
     * @brief 注册SendFile事件（零拷贝发送文件）
     * @param controller IO控制器
     * @return 1表示立即完成，0表示已注册等待，<0表示错误
     */
    virtual int addSendFile(IOController* controller) = 0;

    /**
     * @brief 注册组合式序列 IO 事件
     * @param controller IO 控制器
     * @return 1 表示序列立即完成，0 表示已登记等待，<0 表示注册失败
     * @note 主要服务于需要 READ/WRITE 配对推进的 Sequence awaitable
     */
    virtual int addSequence(IOController* controller) = 0;

    /**
     * @brief 删除fd的所有事件
     * @param controller IO控制器
     * @return 0 表示删除成功，<0 表示删除失败
     */
    virtual int remove(IOController* controller) = 0;

    /**
     * @brief 查询调度器最近一次内部错误
     * @return 如果存在内部错误则返回 IOError；否则返回 std::nullopt
     */
    virtual std::optional<IOError> lastError() const {
        return std::nullopt;
    }


    /**
     * @brief 替换调度器的定时器管理器
     * @param manager 新的时间轮定时器管理器（右值引用）
     * @details 用于自定义时间轮配置（wheelSize、tickDuration）以适应不同的超时场景
     * @note 应在调度器启动前调用，避免运行时替换导致定时器丢失
     * @example
     * ```cpp
     * // 创建适合短超时场景的时间轮（60秒范围，1秒精度）
     * TimingWheelTimerManager manager(60, 1000000000ULL);
     * scheduler->replaceTimerManager(std::move(manager));
     * ```
     */
     void replaceTimerManager(TimingWheelTimerManager&& manager) {
        m_timer_manager = std::move(manager);
    }

    /**
     * @brief 注册定时器
     * @details 添加任务量不大且数量不是特别多的定时任务，如IO超时，sleep等
     * @param timer 定时器共享指针
     * @return true 定时器已加入当前调度器的时间轮；false 插入失败
     */
    bool addTimer(Timer::ptr timer) override {
        return m_timer_manager.push(timer);
    }

    /**
     * @brief 收到 Runtime 的 steal-domain 视图
     */
    virtual void configureStealDomain(std::span<IOScheduler* const> siblings,
                                      size_t self_index)
    {
        m_steal_domain_siblings = siblings;
        m_steal_domain_self_index = self_index;
    }

    /**
     * @brief 暴露给 sibling stealing 的 worker state 入口
     * @details 第一版仅用于同一 Runtime 内的 IOScheduler work-stealing。
     */
    virtual IOSchedulerWorkerState* stealWorkerState() noexcept
    {
        return nullptr;
    }

    /**
     * @brief 返回 stealing 统计快照
     * @details 结果只在 runtime 已停止或外部同步后有定义。
     */
    virtual IOSchedulerStealStats stealStats() const noexcept
    {
        return {};
    }

protected:
    TimingWheelTimerManager m_timer_manager;
    std::span<IOScheduler* const> m_steal_domain_siblings{};
    size_t m_steal_domain_self_index = 0;
};

inline bool IOSchedulerWorkerState::trySteal() {
    if (!stealing_enabled || hasLocalWork() || hasPendingInjected() || siblings.size() <= 1) {
        return false;
    }

    const size_t local_capacity = local_ring.remainingCapacity();
    if (local_capacity == 0) {
        return false;
    }

    ++steal_attempts;
    std::uniform_int_distribution<size_t> start_dist(0, siblings.size() - 1);
    const size_t start = start_dist(random_seed);

    for (size_t probe = 0; probe < siblings.size(); ++probe) {
        const size_t victim_index = (start + probe) % siblings.size();
        if (victim_index == self_index) {
            continue;
        }

        IOScheduler* const victim_scheduler = siblings[victim_index];
        if (victim_scheduler == nullptr) {
            continue;
        }

        IOSchedulerWorkerState* const victim = victim_scheduler->stealWorkerState();
        if (victim == nullptr) {
            continue;
        }

        size_t stolen = 0;
        const size_t victim_size = victim->local_ring.size();
        if (victim_size > 0) {
            const size_t steal_target =
                std::min(local_capacity, std::max<size_t>(1, victim_size / 2));
            for (; stolen < steal_target; ++stolen) {
                TaskRef task;
                if (!victim->stealFront(task)) {
                    break;
                }
                if (!local_ring.push_back(std::move(task))) {
                    if (auto* scheduler = task.belongScheduler()) {
                        scheduler->schedule(std::move(task));
                    }
                    break;
                }
            }
        } else if (victim->hasOwnerDrainedInjected() && victim->hasPendingInjected()) {
            size_t attempts = 0;
            while (stolen < local_capacity && attempts < local_capacity) {
                ++attempts;
                TaskRef task;
                if (!victim->inject_queue.try_dequeue(task)) {
                    break;
                }
                victim->injected_outstanding.fetch_sub(1, std::memory_order_acq_rel);
                if (!task.isValid()) {
                    continue;
                }
                auto* const state = task.state();
                if (state != nullptr &&
                    state->m_resume_owner_only.load(std::memory_order_acquire)) {
                    if (auto* scheduler = task.belongScheduler()) {
                        scheduler->schedule(std::move(task));
                    }
                    continue;
                }
                if (!local_ring.push_back(std::move(task))) {
                    if (auto* scheduler = task.belongScheduler()) {
                        scheduler->schedule(std::move(task));
                    }
                    break;
                }
                ++stolen;
            }
        }

        if (stolen == 0) {
            continue;
        }

        if (stolen > 0) {
            lifo_enabled = true;
            consecutive_lifo_polls = 0;
            polls_since_inject = 0;
            ++steal_successes;
            return true;
        }
    }

    return false;
}


inline bool IOController::fillAwaitable(IOEventType type, void* awaitable) {
    constexpr uint32_t kReadSlotMask =
        static_cast<uint32_t>(IOEventType::ACCEPT) |
        static_cast<uint32_t>(IOEventType::RECV) |
        static_cast<uint32_t>(IOEventType::READV) |
        static_cast<uint32_t>(IOEventType::FILEREAD) |
        static_cast<uint32_t>(IOEventType::FILEWATCH) |
        static_cast<uint32_t>(IOEventType::RECVFROM);
    constexpr uint32_t kWriteSlotMask =
        static_cast<uint32_t>(IOEventType::CONNECT) |
        static_cast<uint32_t>(IOEventType::SEND) |
        static_cast<uint32_t>(IOEventType::WRITEV) |
        static_cast<uint32_t>(IOEventType::SENDFILE) |
        static_cast<uint32_t>(IOEventType::FILEWRITE) |
        static_cast<uint32_t>(IOEventType::SENDTO);

    switch (type) {
    case IOEventType::RECV:
        m_type = static_cast<IOEventType>(
            (static_cast<uint32_t>(m_type) & ~kReadSlotMask) |
            static_cast<uint32_t>(type));
        m_awaitable[READ] = awaitable;
        break;
    case IOEventType::READV:
    case IOEventType::FILEREAD:
    case IOEventType::RECVFROM:
    case IOEventType::FILEWATCH:
        m_type = static_cast<IOEventType>(
            (static_cast<uint32_t>(m_type) & ~kReadSlotMask) |
            static_cast<uint32_t>(type));
        m_awaitable[READ] = awaitable;
#ifdef USE_IOURING
        advanceSqeGeneration(READ);
#endif
        break;
    case IOEventType::ACCEPT:
        m_type = static_cast<IOEventType>(
            (static_cast<uint32_t>(m_type) & ~kReadSlotMask) |
            static_cast<uint32_t>(type));
        m_awaitable[READ] = awaitable;
        break;
    case IOEventType::SEQUENCE:
        m_type |= type;
#ifdef USE_IOURING
        m_awaitable[READ] = awaitable;
        advanceSqeGeneration(READ);
#endif
        break;
    case IOEventType::SEND:
    case IOEventType::WRITEV:
    case IOEventType::SENDFILE:
    case IOEventType::FILEWRITE:
    case IOEventType::SENDTO:
    case IOEventType::CONNECT:
        m_type = static_cast<IOEventType>(
            (static_cast<uint32_t>(m_type) & ~kWriteSlotMask) |
            static_cast<uint32_t>(type));
        m_awaitable[WRITE] = awaitable;
#ifdef USE_IOURING
        advanceSqeGeneration(WRITE);
#endif
        break;
    default:
        return false;
    }
    return true;
}

inline void IOController::removeAwaitable(IOEventType type) {
    m_type &= ~type;
    switch (type) {
    case IOEventType::RECV:
        m_awaitable[READ] = nullptr;
        break;
    case IOEventType::READV:
    case IOEventType::FILEREAD:
    case IOEventType::RECVFROM:
    case IOEventType::FILEWATCH:
        m_awaitable[READ] = nullptr;
#ifdef USE_IOURING
        advanceSqeGeneration(READ);
#endif
        break;
    case IOEventType::ACCEPT:
        m_awaitable[READ] = nullptr;
        break;
    case IOEventType::SEQUENCE:
#ifdef USE_IOURING
        m_awaitable[READ] = nullptr;
        advanceSqeGeneration(READ);
#endif
        break;
    case IOEventType::SEND:
    case IOEventType::WRITEV:
    case IOEventType::SENDFILE:
    case IOEventType::FILEWRITE:
    case IOEventType::SENDTO:
    case IOEventType::CONNECT:
        m_awaitable[WRITE] = nullptr;
#ifdef USE_IOURING
        advanceSqeGeneration(WRITE);
#endif
        break;
    default:
        break;
    }
}   



}

#endif
