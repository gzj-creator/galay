/**
 * @file bounded_channel.h
 * @brief MPMC 有界异步通道实现
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 通道使用 Vyukov 风格的有界环形队列作为数据通路，使用无锁等待队列
 * 管理挂起的发送者和接收者。满或空时，异步操作只挂起协程，不阻塞调度器线程。
 */

#ifndef GALAY_CONCURRENCY_MPMC_BOUNDED_CHANNEL_H
#define GALAY_CONCURRENCY_MPMC_BOUNDED_CHANNEL_H

#include "../../common/error.h"
#include "../../core/waker.h"
#include "../../core/timeout.hpp"
#include "../../../galay-utils/common/defn.hpp"
#include <coroutine>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <emmintrin.h>
#endif
#include <bit>
#include <concepts>
#include <concurrentqueue/moodycamel/concurrentqueue.h>
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <expected>
#include <memory>
#include <new>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace galay::mpmc
{

using kernel::IOError;
using kernel::TimeoutSupport;
using kernel::TimeoutTimer;
using kernel::Waker;
using kernel::WithTimeout;
using kernel::kClosed;
using kernel::kNotReady;
using kernel::kTimeout;

/**
 * @brief 约束 BoundedChannel 可存储的元素类型。
 * @tparam T 元素类型；必须可移动，且移动构造不得抛出异常。
 */
template <typename T>
concept BoundedValue = std::movable<T> &&
    std::is_nothrow_move_constructible_v<T>;

namespace detail {

/**
 * @brief 执行一次平台相关的短时 CPU 自旋提示。
 * @note 该函数不阻塞线程，也不主动把执行权交给操作系统调度器。
 */
inline void boundedChannelCpuPause() noexcept
{
#if defined(_MSC_VER)
    YieldProcessor();
#elif defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__APPLE__) && defined(__aarch64__)
    // Apple LLVM/Rust 的 spin_loop 使用 ISB；b23 的 4P4C 压测证明，YIELD hint
    // 会在高竞争下过度让步，显著增加失败调用和调度反馈。
    __asm__ __volatile__("isb" ::: "memory");
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    // 未提供平台 pause 指令时，退避窗口仍然只有数条指令。
#endif
}

/**
 * @brief 有界 ring 在 CAS 竞争和发布窗口中的自适应退避。
 * @details spin() 仅执行有上限的 CPU hint，不进入调度器；snooze() 只用于等待
 *          slot sequence 更新，持续等待时才交出时间片。
 */
class BoundedChannelBackoff
{
public:
    /**
     * @brief 对 cursor CAS 竞争执行有上限的指数自旋。
     * @note 仅发出 CPU hint，不调用 std::this_thread::yield()。
     */
    void spin() noexcept
    {
        const uint32_t step = m_step < kSpinLimit ? m_step : kSpinLimit;
        const uint32_t spins = 1U << step;
        for (uint32_t i = 0; i < spins; ++i) {
            boundedChannelCpuPause();
        }
        if (m_step <= kSpinLimit) {
            ++m_step;
        }
    }

    /**
     * @brief 等待 slot sequence 发布时执行分级退避。
     * @note 初始阶段使用 CPU hint；持续竞争超过自旋上限后才让出当前线程时间片。
     */
    void snooze() noexcept
    {
        if (m_step <= kSpinLimit) {
            const uint32_t spins = 1U << m_step;
            for (uint32_t i = 0; i < spins; ++i) {
                boundedChannelCpuPause();
            }
        } else {
            std::this_thread::yield();
        }
        if (m_step < kRetryLimit) {
            ++m_step;
        }
    }

private:
    static constexpr uint32_t kSpinLimit = 6;
    static constexpr uint32_t kRetryLimit = 10;
    uint32_t m_step = 0;
};

} // namespace detail

enum class BoundedWaiterState : uint8_t {
    kWaiting,       ///< 已注册但尚未被认领
    kCancelled,     ///< 已由超时路径取消
    kFulfilling,    ///< 对端已认领，正在搬运值
    kFulfilled,     ///< 已成功交接
    kClosed,        ///< 因通道关闭结束
    kFailed         ///< 等待队列无法接收 waiter，返回 kNotReady
};

enum class BoundedWaiterProgress : uint8_t {
    kWaiting,    ///< waiter 尚未完成并已重新进入等待队列。
    kCompleted,  ///< waiter 已结束，当前扫描方应停止。
    kSkipped     ///< waiter 已结束但未消费当前资源事件，扫描方应继续。
};

/**
 * @brief BoundedChannel 的异步等待体。
 * @tparam T 通道元素类型。
 * @details 接收者使用 value 保存交接到的消息；发送者使用 value 保存待发送消息。
 */
template <BoundedValue T>
struct BoundedChannelWaiter
{
    std::optional<T> value;
    std::shared_ptr<BoundedChannelWaiter<T>> deferredWakeNext;
    kernel::detail::DeferredWaker localWaker;
    const TimeoutTimer::ptr timeoutTimer; ///< 发布后只读，避免 stale entry 与完成方并发修改。
    kernel::detail::DeferredWaker* const completionWaker;
    std::atomic<BoundedWaiterState> state{BoundedWaiterState::kWaiting};
    std::atomic<bool> queued{false};
    bool deferredTimeoutWake{false};

    /**
     * @brief 创建绑定指定协程唤醒器的等待体。
     * @param waiter_waker 等待操作完成时使用的唤醒器，所有权移入 waiter。
     */
    explicit BoundedChannelWaiter(Waker waiter_waker,
                                  TimeoutTimer::ptr timeout_timer = {})
        : localWaker(timeout_timer ? Waker() : std::move(waiter_waker))
        , timeoutTimer(std::move(timeout_timer))
        , completionWaker(timeoutTimer != nullptr
              ? &timeoutTimer->completionWaker()
              : &localWaker)
    {
    }
};

template <BoundedValue T>
class BoundedChannel;

struct BoundedChannelTestAccess;

template <BoundedValue T>
class BoundedSendAwaitable;

template <BoundedValue T>
class BoundedRecvAwaitable;

template <BoundedValue T>
class BoundedRecvBatchAwaitable;

/**
 * @brief 异步发送等待体。
 * @tparam T 通道元素类型。
 * @details 满时挂起协程而不阻塞线程；结果通过 std::expected<void, IOError> 返回。
 */
template <BoundedValue T>
class BoundedSendAwaitable : public TimeoutSupport<BoundedSendAwaitable<T>>
{
public:
    /**
     * @brief 创建异步发送等待体。
     * @param channel 目标通道的非拥有指针，等待体完成前必须保持有效。
     * @param value 待发送值，所有权移入等待体。
     */
    explicit BoundedSendAwaitable(BoundedChannel<T>* channel, T&& value)
        : m_channel(channel), m_value(std::move(value)) {}

    /**
     * @brief 尝试在不挂起协程的情况下完成发送。
     * @return 已发送或通道已关闭时返回 true；需要注册发送等待者时返回 false。
     */
    bool await_ready() noexcept;

    /**
     * @brief 注册发送等待者，并在注册后重新检查发送或关闭状态。
     * @tparam Promise 调用方协程 promise 类型。
     * @param handle 当前调用方的协程句柄。
     * @return 需要等待接收方释放容量时返回 true；已同步完成或失败时返回 false。
     * @note 该函数只挂起协程，不阻塞调度器线程。
     */
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) noexcept;

    /**
     * @brief 获取异步发送结果。
     * @return 成功返回空 expected；关闭、超时或等待队列失败分别返回
     *         IOError(kClosed)、IOError(kTimeout) 或 IOError(kNotReady)。
     */
    std::expected<void, IOError> await_resume() noexcept;

    /**
     * @brief 由超时设施尝试取消尚未被对端认领的发送等待者。
     * @note 已进入值搬运阶段的 waiter 不会被超时路径抢占。
     */
    void markTimeout() noexcept;

private:
    friend struct WithTimeout<BoundedSendAwaitable<T>>;

    void bindTimeoutTimer(const TimeoutTimer::ptr& timer) noexcept
    {
        m_timeoutTimer = timer;
    }

    BoundedChannel<T>* m_channel;
    T m_value;
    TimeoutTimer::ptr m_timeoutTimer;
    std::shared_ptr<BoundedChannelWaiter<T>> m_waiter;
    bool m_sent = false;
    bool m_timedOut = false;
};

/**
 * @brief 异步单条接收等待体。
 * @tparam T 通道元素类型。
 * @details 空时挂起协程而不阻塞线程；关闭和超时通过 IOError 返回。
 */
template <BoundedValue T>
class BoundedRecvAwaitable : public TimeoutSupport<BoundedRecvAwaitable<T>>
{
public:
    /**
     * @brief 创建异步单条接收等待体。
     * @param channel 目标通道的非拥有指针，等待体完成前必须保持有效。
     */
    explicit BoundedRecvAwaitable(BoundedChannel<T>* channel)
        : m_channel(channel) {}

    /**
     * @brief 尝试在不挂起协程的情况下接收一条消息。
     * @return 已取得消息或通道已关闭时返回 true；需要注册接收等待者时返回 false。
     */
    bool await_ready() noexcept;

    /**
     * @brief 注册接收等待者，并在注册后重新检查数据或关闭状态。
     * @tparam Promise 调用方协程 promise 类型。
     * @param handle 当前调用方的协程句柄。
     * @return 需要等待生产者提供消息时返回 true；已同步完成或失败时返回 false。
     * @note 该函数只挂起协程，不阻塞调度器线程。
     */
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) noexcept;

    /**
     * @brief 获取异步单条接收结果。
     * @return 成功返回收到的消息；关闭、超时或等待队列失败分别返回
     *         IOError(kClosed)、IOError(kTimeout) 或 IOError(kNotReady)。
     */
    std::expected<T, IOError> await_resume() noexcept;

    /**
     * @brief 由超时设施尝试取消尚未被对端认领的接收等待者。
     * @note 已进入值搬运阶段的 waiter 不会被超时路径抢占。
     */
    void markTimeout() noexcept;

private:
    friend struct WithTimeout<BoundedRecvAwaitable<T>>;

    void bindTimeoutTimer(const TimeoutTimer::ptr& timer) noexcept
    {
        m_timeoutTimer = timer;
    }

    BoundedChannel<T>* m_channel;
    TimeoutTimer::ptr m_timeoutTimer;
    std::optional<T> m_ready;
    std::shared_ptr<BoundedChannelWaiter<T>> m_waiter;
    bool m_timedOut = false;
};

/**
 * @brief 异步批量接收等待体。
 * @tparam T 通道元素类型。
 * @details 等待至少一条消息，恢复时尽量补齐请求数量；不会阻塞调度器线程。
 */
template <BoundedValue T>
class BoundedRecvBatchAwaitable : public TimeoutSupport<BoundedRecvBatchAwaitable<T>>
{
public:
    /**
     * @brief 创建异步批量接收等待体。
     * @param channel 目标通道的非拥有指针，等待体完成前必须保持有效。
     * @param count 单次最多接收的消息数；0 表示立即返回空批次。
     */
    BoundedRecvBatchAwaitable(BoundedChannel<T>* channel, size_t count)
        : m_channel(channel), m_count(count) {}

    /**
     * @brief 尝试在不挂起协程的情况下取得一批消息。
     * @return 已取得批次或通道已关闭时返回 true；需要注册接收等待者时返回 false。
     */
    bool await_ready() noexcept;

    /**
     * @brief 注册批量接收等待者，并在注册后重新检查数据或关闭状态。
     * @tparam Promise 调用方协程 promise 类型。
     * @param handle 当前调用方的协程句柄。
     * @return 需要等待至少一条消息时返回 true；已同步完成或失败时返回 false。
     * @note 该函数只挂起协程，不阻塞调度器线程。
     */
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) noexcept;

    /**
     * @brief 获取异步批量接收结果。
     * @return 成功返回最多 count 条消息；关闭、超时或等待队列失败分别返回
     *         IOError(kClosed)、IOError(kTimeout) 或 IOError(kNotReady)。
     */
    std::expected<std::vector<T>, IOError> await_resume() noexcept;

    /**
     * @brief 由超时设施尝试取消尚未被对端认领的批量接收等待者。
     * @note 已进入值搬运阶段的 waiter 不会被超时路径抢占。
     */
    void markTimeout() noexcept;

private:
    friend struct WithTimeout<BoundedRecvBatchAwaitable<T>>;

    void bindTimeoutTimer(const TimeoutTimer::ptr& timer) noexcept
    {
        m_timeoutTimer = timer;
    }

    BoundedChannel<T>* m_channel;
    TimeoutTimer::ptr m_timeoutTimer;
    size_t m_count;
    std::optional<std::vector<T>> m_ready;
    std::shared_ptr<BoundedChannelWaiter<T>> m_waiter;
    bool m_timedOut = false;
};

/**
 * @brief 固定容量、线程安全的 MPMC 异步通道。
 * @tparam T 元素类型，要求可移动且移动构造不抛异常。
 *
 * @details
 * - 容量会向上取整为不小于 2 的 2 的幂，且不超过 cursor 半区间上限 2^62；
 *   可通过 capacity() 获取实际容量。
 * - trySend() / tryRecv() 不等待 Full/Empty 条件；同侧 cursor 竞争会在当前调用内
 *   通过 CPU hint 退避重试，不主动进入调度器。
 * - send() / recv() / recvBatch() 满或空时只挂起协程，不阻塞底层线程。
 * - 多生产者之间不保证全局顺序；每个生产者自身发送的消息保持 FIFO。
 *
 * @note 通道是身份对象，不可复制或移动。通道必须存活到所有挂起操作完成或超时。
 * @note trySend(T&&) 只有在成功抢到 slot 后才移动参数；失败时参数保持未移动。
 *       需要失败后拿回值重试时应使用 trySend()。co_await send() 的值会先移动到等待体，
 *       超时或关闭时不会归还。
 */
template <BoundedValue T>
class BoundedChannel
{
public:
    static_assert(BoundedValue<T>,
                  "BoundedChannel requires nothrow move construction");

    /**
     * @brief 构造有界通道。
     * @param capacity 期望容量；小于等于 2 时实际容量为 2，其余向上取整到 2 的幂，
     *                 最大钳制到平台可表示范围与 2^62 中的较小值。
     * @note 构造完成后可由多个生产者和消费者线程并发访问。
     */
    explicit BoundedChannel(size_t capacity)
        : m_capacity(normalizeCapacity(capacity))
        , m_mask(m_capacity - 1)
        , m_slots(m_capacity)
    {
        for (size_t i = 0; i < m_capacity; ++i) {
            m_slots[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    /**
     * @brief 销毁 ring 中尚未消费的已发布消息并释放通道资源。
     * @pre 不得再有并发调用方或挂起在该通道上的 awaitable。
     * @note 析构过程不负责关闭通道或等待其他线程退出。
     */
    ~BoundedChannel() noexcept
    {
        const uint64_t head = m_head.load(std::memory_order_relaxed);
        const uint64_t tail =
            m_tail.load(std::memory_order_relaxed) & kTailPositionMask;
        const uint64_t pending = tail >= head ? tail - head : 0;
        const size_t count = pending < m_capacity
            ? static_cast<size_t>(pending)
            : m_capacity;
        for (size_t i = 0; i < count; ++i) {
            const uint64_t position = head + i;
            Slot& slot = m_slots[position & m_mask];
            if (slot.sequence.load(std::memory_order_relaxed) == position + 1) {
                std::destroy_at(slot.value());
            }
        }
    }

    /** @brief 禁止复制构造；通道具有唯一身份。 */
    BoundedChannel(const BoundedChannel&) = delete;

    /** @brief 禁止复制赋值；通道具有唯一身份。 */
    BoundedChannel& operator=(const BoundedChannel&) = delete;

    /** @brief 禁止移动构造，避免使已注册 waiter 持有失效地址。 */
    BoundedChannel(BoundedChannel&&) = delete;

    /** @brief 禁止移动赋值，避免使已注册 waiter 持有失效地址。 */
    BoundedChannel& operator=(BoundedChannel&&) = delete;

    /**
     * @brief 尝试立即发送一条消息。
     * @param value 待发送消息；只有发送成功时才会被移动。
     * @return true 表示发送成功；false 表示通道已关闭或当前已满。
     * @note CAS 竞争在当前调用内通过 CPU hint 重试，不因输掉 cursor 认领而返回 false。
     */
    bool trySend(T&& value)
    {
        const auto result = ringEnqueueResult(std::move(value));
        if (result == RingEnqueueResult::kSent) {
            if (m_recvWaiterPathUsed.load(std::memory_order_seq_cst)) {
                requestPump(kRecvWork);
            }
            return true;
        }
        if (result == RingEnqueueResult::kClosedAndNotify) {
            requestPump(static_cast<uint8_t>(kRecvWork | kSendWork));
        }
        return false;
    }

    /**
     * @brief 复制并尝试立即发送一条消息。
     * @param value 待复制消息。
     * @return true 表示发送成功；false 表示通道已关闭或当前已满。
     * @note 先创建本地副本，再复用移动发送路径；发送失败不会修改原值。
     */
    bool trySend(const T& value)
        requires std::copy_constructible<T> &&
            std::is_nothrow_copy_constructible_v<T>
    {
        T copy = value;
        return trySend(std::move(copy));
    }

    /**
     * @brief 异步发送一条消息。
     * @param value 待发送消息；会移动进等待体。
     * @return 可 co_await 的等待体；关闭返回 IOError(kClosed, 0)，超时返回 IOError(kTimeout, 0)。
     * @note 满时挂起协程而不阻塞调度器线程；通道必须在等待期间保持有效。
     */
    BoundedSendAwaitable<T> send(T&& value);

    /**
     * @brief 尝试立即接收一条消息。
     * @return 有消息时返回消息；为空时返回 std::nullopt。关闭不影响排空残留消息。
     * @note CAS 竞争在当前调用内通过 CPU hint 重试，不因输掉 cursor 认领而返回空结果。
     */
    std::optional<T> tryRecv()
    {
        std::optional<T> value;
        if (ringDequeueTo([&value](T&& item) {
                value.emplace(std::move(item));
            })) {
            if (m_sendWaiterPathUsed.load(std::memory_order_seq_cst)) {
                requestPump(kSendWork);
            }
            return value;
        }
        return std::nullopt;
    }

    /**
     * @brief 异步接收一条消息。
     * @return 可 co_await 的等待体；关闭且已排空时返回 IOError(kClosed, 0)，超时返回 IOError(kTimeout, 0)。
     * @note 空时挂起协程而不阻塞调度器线程；通道必须在等待期间保持有效。
     */
    BoundedRecvAwaitable<T> recv();

    /**
     * @brief 异步批量接收消息。
     * @param count 单次最多接收的消息数。
     * @return 可 co_await 的等待体；至少收到一条后尽量补齐至 count 条。
     * @note count 为 0 时立即返回空批次；等待期间不会阻塞调度器线程。
     */
    BoundedRecvBatchAwaitable<T> recvBatch(size_t count);

    /**
     * @brief 尝试批量接收消息。
     * @param count 单次最多接收的消息数；0 返回空 vector。
     * @return 收到至少一条消息时返回消息批次；否则返回 std::nullopt。
     * @note 该函数连续调用 tryRecv()，不会等待后续消息来补齐 count。
     */
    std::optional<std::vector<T>> tryRecvBatch(size_t count)
    {
        if (count == 0) {
            return std::vector<T>{};
        }

        std::vector<T> values;
        values.reserve(count);
        while (values.size() < count) {
            auto value = tryRecv();
            if (!value.has_value()) {
                break;
            }
            values.push_back(std::move(*value));
        }
        if (values.empty()) {
            return std::nullopt;
        }
        return values;
    }

    /**
     * @brief 关闭通道并唤醒所有等待者。
     * @details close 与 producer reservation 在线性化于同一个 tail 原子。先完成
     *          reservation 的发送仍会发布且可被接收；先发布 close 的发送会失败。
     *          接收方仅在所有在先 reservation 已发布并被认领后返回 kClosed。
     * @note 可与发送、接收及其他 close() 并发执行且操作幂等；关闭后仍会排空残留消息。
     */
    void close() noexcept
    {
        const uint64_t previous =
            m_tail.fetch_or(kTailClosedBit, std::memory_order_acq_rel);
        if ((previous & kTailClosedBit) != 0) {
            return;
        }
        requestPump(static_cast<uint8_t>(kRecvWork | kSendWork));
    }

    /**
     * @brief 查询通道是否已关闭。
     * @return true 表示已关闭。
     * @note 返回调用时刻的 close 快照，不表示所有在先 reservation 已发布并排空。
     *       存在并发发送时，不可用 tryRecv() 为空且 isClosed() 为 true 推断 drain 完成；
     *       异步 recv()/recvBatch() 会在排空后返回 kClosed。
     */
    bool isClosed() const noexcept
    {
        return (m_tail.load(std::memory_order_acquire) & kTailClosedBit) != 0;
    }

    /**
     * @brief 返回实际生效容量。
     * @return 取整后的 2 的幂容量。
     * @note 容量在构造后不再变化，可由任意线程读取。
     */
    size_t capacity() const noexcept
    {
        return m_capacity;
    }

    /**
     * @brief 返回 ring 中的近似消息数。
     * @return 仅供诊断，不可用作同步条件。
     */
    size_t size() const noexcept
    {
        const uint64_t tail =
            m_tail.load(std::memory_order_relaxed) & kTailPositionMask;
        const uint64_t head = m_head.load(std::memory_order_relaxed);
        if (tail < head) {
            return 0;
        }
        const uint64_t count = tail - head;
        return count < m_capacity ? static_cast<size_t>(count) : m_capacity;
    }

    /**
     * @brief 近似检查 ring 是否为空。
     * @return 仅供诊断，不可用作同步条件。
     */
    bool empty() const noexcept
    {
        return size() == 0;
    }

    /**
     * @brief 近似检查 ring 是否已满。
     * @return 仅供诊断，不可用作同步条件。
     */
    bool full() const noexcept
    {
        return size() >= m_capacity;
    }

private:
    // AArch64 的隔离粒度是 128B，其他架构是 64B。取 64 会让 m_head/m_tail
    // 落在同一条 128B 行上，生产者与消费者的热索引持续互相无效化。
    static constexpr uint64_t kTailClosedBit = uint64_t{1} << 63U;
    static constexpr uint64_t kTailPositionMask = kTailClosedBit - 1U;
    static constexpr uint64_t kMaxRingCapacity = uint64_t{1} << 62U;

    struct Slot
    {
        alignas(T) std::byte storage[sizeof(T)];
        std::atomic<uint64_t> sequence{0};
        // storage 在 sequence 前，避免高对齐 T 被小成员打断。

        /**
         * @brief 获取 slot 原始存储对应的 T 指针。
         * @return 经 std::launder 处理的存储地址，用于 construct_at、访问与 destroy_at。
         */
        T* value() noexcept
        {
            return std::launder(reinterpret_cast<T*>(storage));
        }
    };

    using Waiter = BoundedChannelWaiter<T>;
    using WaiterPtr = std::shared_ptr<Waiter>;
    using WaiterQueue = moodycamel::ConcurrentQueue<WaiterPtr>;

    static constexpr uint8_t kPumpRunning = 0x01;
    static constexpr uint8_t kRecvWork = 0x02;
    static constexpr uint8_t kSendWork = 0x04;

    /**
     * @brief 把已完成 waiter 追加到 pump 的本地延迟唤醒链。
     * @details pump 释放 channel ownership 后才发出实际唤醒，避免 inline resume
     *          销毁 channel 后 pump 继续访问成员。
     */
    static void appendDeferredWake(WaiterPtr& wakeHead,
                                   WaiterPtr& wakeTail,
                                   const WaiterPtr& waiter,
                                   bool timeoutWinner) noexcept
    {
        waiter->deferredWakeNext.reset();
        waiter->deferredTimeoutWake = timeoutWinner;
        if (wakeTail) {
            wakeTail->deferredWakeNext = waiter;
        } else {
            wakeHead = waiter;
        }
        wakeTail = waiter;
    }

    /**
     * @brief 在 channel 不再被访问后发出 pump 收集的全部唤醒。
     * @note 任一 wake 都可能 inline 恢复并销毁 channel，因此该函数不得访问 this。
     */
    static void issueDeferredWakes(WaiterPtr wakeHead) noexcept
    {
        while (wakeHead) {
            WaiterPtr next = std::move(wakeHead->deferredWakeNext);
            const bool timeoutWinner = wakeHead->deferredTimeoutWake;
            wakeHead->deferredTimeoutWake = false;
            if (timeoutWinner && wakeHead->timeoutTimer != nullptr) {
                wakeHead->timeoutTimer->wakeTimeoutWinner();
            } else if (!wakeHead->completionWaker->requestWake()) {
                // timeout 或其他完成方已经发布同一 waiter 的唤醒。
            }
            wakeHead = std::move(next);
        }
    }

    /** @brief 尝试由唯一 pump owner 完成 kWaiting -> kFulfilling 认领。 */
    bool claimWaiter(const WaiterPtr& waiter) noexcept
    {
        BoundedWaiterState expected = BoundedWaiterState::kWaiting;
        return waiter->state.compare_exchange_strong(
            expected,
            BoundedWaiterState::kFulfilling,
            std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    /** @brief 保证 waiter 在指定队列中至多保留一个可发现入口。 */
    bool enqueueWaiter(WaiterQueue& waiters, const WaiterPtr& waiter) noexcept
    {
        bool expected = false;
        if (!waiter->queued.compare_exchange_strong(expected,
                                                    true,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_acquire)) {
            return true;
        }
        if (waiters.enqueue(waiter)) {
            return true;
        }
        waiter->queued.store(false, std::memory_order_release);
        return false;
    }

    /** @brief 从指定队列移除一个有效入口，并在认领前清除其队列成员位。 */
    bool tryDequeueWaiter(WaiterQueue& waiters, WaiterPtr& waiter) noexcept
    {
        while (waiters.try_dequeue(waiter)) {
            if (waiter && waiter->queued.exchange(false, std::memory_order_acq_rel)) {
                return true;
            }
        }
        return false;
    }

    void synchronizeRecvWaiterPath() noexcept
    {
        const uint64_t position = m_head.load(std::memory_order_relaxed);
        // 不可弱化：与生产者的 SC sequence.store -> waiterPath.load 组成 Dekker 握手。
        [[maybe_unused]] const uint64_t published =
            m_slots[position & m_mask].sequence.load(std::memory_order_seq_cst);
    }

    void synchronizeSendWaiterPath() noexcept
    {
        const uint64_t position =
            m_tail.load(std::memory_order_relaxed) & kTailPositionMask;
        // 不可弱化：与消费者的 SC sequence.store -> waiterPath.load 组成 Dekker 握手。
        [[maybe_unused]] const uint64_t released =
            m_slots[position & m_mask].sequence.load(std::memory_order_seq_cst);
    }

    /** @brief close 已发布且所有在先 reservation 均已被消费者认领时返回 true。 */
    bool isClosedAndDrained() const noexcept
    {
        const uint64_t tail = m_tail.load(std::memory_order_acquire);
        return (tail & kTailClosedBit) != 0 &&
            (tail & kTailPositionMask) ==
                m_head.load(std::memory_order_acquire);
    }

    /**
     * @brief 将请求容量规范化为 ring 所需的 2 的幂。
     * @param capacity 调用方请求的容量。
     * @return 不小于 2 且不小于请求值的最小 2 的幂。
     */
    static size_t normalizeCapacity(size_t capacity) noexcept
    {
        if (capacity <= 2) {
            return 2;
        }
        // 算法用有符号 64-bit 差值判定 slot 世代，容量必须小于半区间。
        // 超出范围的请求不具备可表示的 ring，钳制后通常会由分配器拒绝。
        const size_t maxCapacity = sizeof(size_t) >= sizeof(uint64_t)
            ? static_cast<size_t>(kMaxRingCapacity)
            : (size_t{1} << (sizeof(size_t) * 8U - 1U));
        if (capacity >= maxCapacity) {
            return maxCapacity;
        }
        return std::bit_ceil(capacity);
    }

    enum class RingEnqueueResult : uint8_t {
        kSent,
        kFull,
        kClosed,
        kClosedAndNotify,
    };

    /**
     * @brief 尝试在 ring 中认领一个生产者 slot 并发布消息。
     * @param value 待发布消息；仅在成功认领 slot 后移动。
     * @return 区分发送成功、ring 已满、通道已关闭及 cursor 耗尽触发关闭。
     * @note tail 的低 63 位是单调且不回绕的 reservation cursor，高位是关闭位。
     *       close() 与生产者 reservation 在线性化于同一个 tail 原子；cursor 到达
     *       2^63-1 时自动关闭，避免位置值与关闭位发生碰撞。
     */
    RingEnqueueResult ringEnqueueResult(T&& value) noexcept
    {
        uint64_t tail = m_tail.load(std::memory_order_relaxed);
        uint64_t position = 0;
        Slot* slot = nullptr;
        detail::BoundedChannelBackoff backoff;
        for (;;) {
            if ((tail & kTailClosedBit) != 0) {
                return RingEnqueueResult::kClosed;
            }
            position = tail;
            if (position == kTailPositionMask) {
                uint64_t expected = position;
                if (m_tail.compare_exchange_weak(
                        expected,
                        position | kTailClosedBit,
                        std::memory_order_acq_rel,
                        std::memory_order_relaxed)) {
                    return RingEnqueueResult::kClosedAndNotify;
                }
                tail = expected;
                continue;
            }
            slot = &m_slots[position & m_mask];
            const uint64_t sequence =
                slot->sequence.load(std::memory_order_acquire);
            const int64_t difference =
                std::bit_cast<int64_t>(sequence - position);
            if (difference == 0) {
                if (m_tail.compare_exchange_weak(tail,
                                                 position + 1,
                                                 std::memory_order_relaxed,
                                                 std::memory_order_relaxed)) {
                    break;
                }
                backoff.spin();
            } else if (difference < 0) {
                return RingEnqueueResult::kFull;
            } else {
                tail = m_tail.load(std::memory_order_relaxed);
                backoff.snooze();
            }
        }

        [[maybe_unused]] T* const stored =
            std::construct_at(slot->value(), std::move(value));
        // construct_at 的返回值只回显目标地址，发布由 sequence 完成。
        slot->sequence.store(position + 1, std::memory_order_seq_cst);
        return RingEnqueueResult::kSent;
    }

    /** @brief 白盒测试兼容入口；公开路径使用 result 区分 close/exhaustion。 */
    bool ringEnqueue(T&& value) noexcept
    {
        const auto result = ringEnqueueResult(std::move(value));
        if (result == RingEnqueueResult::kClosedAndNotify) {
            requestPump(static_cast<uint8_t>(kRecvWork | kSendWork));
        }
        return result == RingEnqueueResult::kSent;
    }

    /**
     * @brief 尝试从 ring 认领一个消费者 slot，并把消息交给调用方处理。
     * @tparam Consume 接收 T&& 的不可抛出消费回调类型。
     * @param consume 在成功认领 slot 后调用一次的消费回调。
     * @return 消费成功返回 true；当前 ring 没有已发布消息时返回 false。
     * @note 回调返回后才销毁 slot 中的对象并发布该 slot 供生产者复用。
     */
    template <typename Consume>
    bool ringDequeueTo(Consume&& consume) noexcept
    {
        uint64_t position = m_head.load(std::memory_order_relaxed);
        Slot* slot = nullptr;
        detail::BoundedChannelBackoff backoff;
        for (;;) {
            slot = &m_slots[position & m_mask];
            const uint64_t sequence =
                slot->sequence.load(std::memory_order_acquire);
            const int64_t difference =
                std::bit_cast<int64_t>(sequence - (position + 1));
            if (difference == 0) {
                if (m_head.compare_exchange_weak(position,
                                                 position + 1,
                                                 std::memory_order_relaxed,
                                                 std::memory_order_relaxed)) {
                    break;
                }
                backoff.spin();
            } else if (difference < 0) {
                return false;
            } else {
                position = m_head.load(std::memory_order_relaxed);
                backoff.snooze();
            }
        }

        consume(std::move(*slot->value()));
        std::destroy_at(slot->value());
        slot->sequence.store(position + m_capacity, std::memory_order_seq_cst);
        return true;
    }

    /**
     * @brief 尝试推进一个发送 waiter 的状态。
     * @param waiter 待推进的共享等待体；空指针视为未认领。
     * @return kCompleted 表示等待体已结束，kWaiting 表示已重新排队，
     *         kSkipped 表示该入口已由其他并发路径处理。
     * @note ring 仍满时会把 waiter 恢复为 kWaiting 并重新入队。
     */
    BoundedWaiterProgress tryCompleteSendWaiter(const WaiterPtr& waiter,
                                                WaiterPtr& wakeHead,
                                                WaiterPtr& wakeTail) noexcept
    {
        if (!waiter) {
            return BoundedWaiterProgress::kSkipped;
        }

        TimeoutTimer* const timeoutTimer = waiter->timeoutTimer.get();
        bool timeoutOperationStarted = false;
        if (timeoutTimer != nullptr) {
            const auto start = timeoutTimer->tryBeginOperation();
            if (start == TimeoutTimer::OperationStart::kBusy) {
                if (waiter->state.load(std::memory_order_acquire) ==
                        BoundedWaiterState::kWaiting &&
                    enqueueWaiter(m_sendWaiters, waiter)) {
                    return BoundedWaiterProgress::kWaiting;
                }
                return BoundedWaiterProgress::kSkipped;
            }
            if (start == TimeoutTimer::OperationStart::kTimeoutWon) {
                BoundedWaiterState expected = BoundedWaiterState::kWaiting;
                if (waiter->state.compare_exchange_strong(
                        expected,
                        BoundedWaiterState::kCancelled,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    waiter->value.reset();
                }
                return BoundedWaiterProgress::kSkipped;
            }
            if (start == TimeoutTimer::OperationStart::kOperationWon) {
                return BoundedWaiterProgress::kSkipped;
            }
            timeoutOperationStarted = true;
        }

        if (!claimWaiter(waiter)) {
            if (timeoutOperationStarted) {
                const auto aborted = timeoutTimer->abortOperation();
                if (aborted == TimeoutTimer::OperationAbort::kTimeoutWon) {
                    BoundedWaiterState expected = BoundedWaiterState::kWaiting;
                    if (waiter->state.compare_exchange_strong(
                            expected,
                            BoundedWaiterState::kCancelled,
                            std::memory_order_acq_rel,
                            std::memory_order_acquire)) {
                        waiter->value.reset();
                    }
                    appendDeferredWake(wakeHead, wakeTail, waiter, true);
                }
            }
            return BoundedWaiterProgress::kSkipped;
        }

        if (!waiter->value.has_value()) {
            const bool committed =
                !timeoutOperationStarted || timeoutTimer->commitOperation();
            waiter->state.store(committed ? BoundedWaiterState::kFailed
                                          : BoundedWaiterState::kCancelled,
                                std::memory_order_release);
            if (committed) {
                appendDeferredWake(wakeHead, wakeTail, waiter, false);
            }
            return BoundedWaiterProgress::kCompleted;
        }

        const auto result = ringEnqueueResult(std::move(*waiter->value));
        if (result == RingEnqueueResult::kSent) {
            waiter->value.reset();
            const bool committed =
                !timeoutOperationStarted || timeoutTimer->commitOperation();
            waiter->state.store(committed ? BoundedWaiterState::kFulfilled
                                          : BoundedWaiterState::kFailed,
                                std::memory_order_release);
            appendDeferredWake(wakeHead, wakeTail, waiter, false);
            requestPump(kRecvWork);
            return BoundedWaiterProgress::kCompleted;
        }

        if (result == RingEnqueueResult::kClosed ||
            result == RingEnqueueResult::kClosedAndNotify) {
            waiter->value.reset();
            const bool committed =
                !timeoutOperationStarted || timeoutTimer->commitOperation();
            waiter->state.store(committed ? BoundedWaiterState::kClosed
                                          : BoundedWaiterState::kFailed,
                                std::memory_order_release);
            appendDeferredWake(wakeHead, wakeTail, waiter, false);
            if (result == RingEnqueueResult::kClosedAndNotify) {
                requestPump(static_cast<uint8_t>(kRecvWork | kSendWork));
            }
            return BoundedWaiterProgress::kCompleted;
        }

        if (!enqueueWaiter(m_sendWaiters, waiter)) {
            const bool committed =
                !timeoutOperationStarted || timeoutTimer->commitOperation();
            waiter->state.store(committed ? BoundedWaiterState::kFailed
                                          : BoundedWaiterState::kCancelled,
                                std::memory_order_release);
            appendDeferredWake(wakeHead, wakeTail, waiter, false);
            return BoundedWaiterProgress::kCompleted;
        }
        waiter->state.store(BoundedWaiterState::kWaiting, std::memory_order_release);

        if (!timeoutOperationStarted) {
            return BoundedWaiterProgress::kWaiting;
        }
        const auto aborted = timeoutTimer->abortOperation();
        if (aborted == TimeoutTimer::OperationAbort::kRearmed) {
            return BoundedWaiterProgress::kWaiting;
        }
        if (aborted == TimeoutTimer::OperationAbort::kTimeoutWon) {
            BoundedWaiterState expected = BoundedWaiterState::kWaiting;
            if (waiter->state.compare_exchange_strong(
                    expected,
                    BoundedWaiterState::kCancelled,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                waiter->value.reset();
            }
            appendDeferredWake(wakeHead, wakeTail, waiter, true);
        }
        return BoundedWaiterProgress::kSkipped;
    }

    /**
     * @brief 尝试推进一个接收 waiter 的状态。
     * @param waiter 待推进的共享等待体；空指针视为未认领。
     * @return kCompleted 表示等待体已结束，kWaiting 表示已重新排队，
     *         kSkipped 表示该入口已由其他并发路径处理。
     * @note ring 仍空时会把 waiter 恢复为 kWaiting 并重新入队。
     */
    BoundedWaiterProgress tryCompleteRecvWaiter(const WaiterPtr& waiter,
                                                WaiterPtr& wakeHead,
                                                WaiterPtr& wakeTail) noexcept
    {
        if (!waiter) {
            return BoundedWaiterProgress::kSkipped;
        }

        TimeoutTimer* const timeoutTimer = waiter->timeoutTimer.get();
        bool timeoutOperationStarted = false;
        if (timeoutTimer != nullptr) {
            const auto start = timeoutTimer->tryBeginOperation();
            if (start == TimeoutTimer::OperationStart::kBusy) {
                if (waiter->state.load(std::memory_order_acquire) ==
                        BoundedWaiterState::kWaiting &&
                    enqueueWaiter(m_recvWaiters, waiter)) {
                    return BoundedWaiterProgress::kWaiting;
                }
                return BoundedWaiterProgress::kSkipped;
            }
            if (start == TimeoutTimer::OperationStart::kTimeoutWon) {
                BoundedWaiterState expected = BoundedWaiterState::kWaiting;
                if (!waiter->state.compare_exchange_strong(
                        expected,
                        BoundedWaiterState::kCancelled,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    // timeout winner 或 terminal owner 已发布最终状态。
                }
                return BoundedWaiterProgress::kSkipped;
            }
            if (start == TimeoutTimer::OperationStart::kOperationWon) {
                return BoundedWaiterProgress::kSkipped;
            }
            timeoutOperationStarted = true;
        }

        if (!claimWaiter(waiter)) {
            if (timeoutOperationStarted) {
                const auto aborted = timeoutTimer->abortOperation();
                if (aborted == TimeoutTimer::OperationAbort::kTimeoutWon) {
                    BoundedWaiterState expected = BoundedWaiterState::kWaiting;
                    if (!waiter->state.compare_exchange_strong(
                            expected,
                            BoundedWaiterState::kCancelled,
                            std::memory_order_acq_rel,
                            std::memory_order_acquire)) {
                        // stale queue entry 已由其他完成方终结。
                    }
                    appendDeferredWake(wakeHead, wakeTail, waiter, true);
                }
            }
            return BoundedWaiterProgress::kSkipped;
        }

        if (ringDequeueTo([&waiter](T&& value) {
                waiter->value.emplace(std::move(value));
            })) {
            const bool committed =
                !timeoutOperationStarted || timeoutTimer->commitOperation();
            waiter->state.store(committed ? BoundedWaiterState::kFulfilled
                                          : BoundedWaiterState::kFailed,
                                std::memory_order_release);
            requestPump(kSendWork);
            appendDeferredWake(wakeHead, wakeTail, waiter, false);
            return BoundedWaiterProgress::kCompleted;
        }

        if (isClosedAndDrained()) {
            const bool committed =
                !timeoutOperationStarted || timeoutTimer->commitOperation();
            waiter->state.store(committed ? BoundedWaiterState::kClosed
                                          : BoundedWaiterState::kFailed,
                                std::memory_order_release);
            appendDeferredWake(wakeHead, wakeTail, waiter, false);
            return BoundedWaiterProgress::kCompleted;
        }

        if (!enqueueWaiter(m_recvWaiters, waiter)) {
            const bool committed =
                !timeoutOperationStarted || timeoutTimer->commitOperation();
            waiter->state.store(committed ? BoundedWaiterState::kFailed
                                          : BoundedWaiterState::kCancelled,
                                std::memory_order_release);
            appendDeferredWake(wakeHead, wakeTail, waiter, false);
            return BoundedWaiterProgress::kCompleted;
        }
        waiter->state.store(BoundedWaiterState::kWaiting, std::memory_order_release);

        if (!timeoutOperationStarted) {
            return BoundedWaiterProgress::kWaiting;
        }
        const auto aborted = timeoutTimer->abortOperation();
        if (aborted == TimeoutTimer::OperationAbort::kRearmed) {
            return BoundedWaiterProgress::kWaiting;
        }
        if (aborted == TimeoutTimer::OperationAbort::kTimeoutWon) {
            BoundedWaiterState expected = BoundedWaiterState::kWaiting;
            if (!waiter->state.compare_exchange_strong(
                    expected,
                    BoundedWaiterState::kCancelled,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                // close 或其他 terminal owner 已完成该 waiter。
            }
            appendDeferredWake(wakeHead, wakeTail, waiter, true);
        }
        return BoundedWaiterProgress::kSkipped;
    }

    /**
     * @brief 发布一类 waiter 慢路径工作，并在无 owner 时成为唯一 pump owner。
     * @param work kRecvWork、kSendWork 或二者的按位组合。
     * @note 数据/队列入口必须先于 release fetch_or 发布。Running 只能由 owner 清除；
     *       requester 与 owner 的退出 CAS 共享同一原子，因此不存在最后检查空窗。
     */
    void requestPump(uint8_t work) noexcept
    {
        uint8_t state = static_cast<uint8_t>(
            m_pumpState.fetch_or(work, std::memory_order_release) | work);
        for (;;) {
            if ((state & kPumpRunning) != 0) {
                return;
            }
            uint8_t expected = state;
            const uint8_t desired = static_cast<uint8_t>(state | kPumpRunning);
            if (m_pumpState.compare_exchange_weak(expected,
                                                  desired,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
                runPump();
                return;
            }
            state = expected;
        }
    }

    /** @brief 推进接收 waiter，直到队列耗尽或首个 live waiter 因无数据重新阻塞。 */
    void drainRecvWaiters(WaiterPtr& wakeHead, WaiterPtr& wakeTail) noexcept
    {
        WaiterPtr waiter;
        while (tryDequeueWaiter(m_recvWaiters, waiter)) {
            const auto progress = tryCompleteRecvWaiter(waiter, wakeHead, wakeTail);
            if (progress == BoundedWaiterProgress::kWaiting) {
                return;
            }
        }
    }

    /** @brief 推进发送 waiter，直到队列耗尽或首个 live waiter 因 ring 满重新阻塞。 */
    void drainSendWaiters(WaiterPtr& wakeHead, WaiterPtr& wakeTail) noexcept
    {
        WaiterPtr waiter;
        while (tryDequeueWaiter(m_sendWaiters, waiter)) {
            const auto progress = tryCompleteSendWaiter(waiter, wakeHead, wakeTail);
            if (progress == BoundedWaiterProgress::kWaiting) {
                return;
            }
        }
    }

    /**
     * @brief 由唯一 owner 领取全部 pending work，推进到固定点后释放 ownership。
     * @note ownership 释放后只访问本地 waiter 链；实际唤醒可安全 inline 销毁 channel。
     */
    void runPump() noexcept
    {
        WaiterPtr wakeHead;
        WaiterPtr wakeTail;
        for (;;) {
            const uint8_t claimed =
                m_pumpState.fetch_and(kPumpRunning, std::memory_order_acq_rel);
            if ((claimed & kRecvWork) != 0) {
                drainRecvWaiters(wakeHead, wakeTail);
            }
            if ((claimed & kSendWork) != 0) {
                drainSendWaiters(wakeHead, wakeTail);
            }

            uint8_t expected = kPumpRunning;
            if (m_pumpState.compare_exchange_strong(expected,
                                                    0,
                                                    std::memory_order_release,
                                                    std::memory_order_acquire)) {
                break;
            }
        }

        issueDeferredWakes(std::move(wakeHead));
    }

    template <BoundedValue U>
    friend class BoundedSendAwaitable;
    template <BoundedValue U>
    friend class BoundedRecvAwaitable;
    template <BoundedValue U>
    friend class BoundedRecvBatchAwaitable;
    friend struct BoundedChannelTestAccess;

    alignas(::galay::utils::kCacheLineSize) std::atomic<uint64_t> m_tail{0};
    alignas(::galay::utils::kCacheLineSize) std::atomic<uint64_t> m_head{0};
    alignas(::galay::utils::kCacheLineSize) std::atomic<uint8_t> m_pumpState{0};
    // Once an async waiter path has been used, keep the waiter-aware path enabled.
    // Before that point, synchronous trySend/tryRecv avoid polling empty waiter queues.
    alignas(::galay::utils::kCacheLineSize)
        std::atomic<bool> m_recvWaiterPathUsed{false};
    alignas(::galay::utils::kCacheLineSize)
        std::atomic<bool> m_sendWaiterPathUsed{false};
    const size_t m_capacity;
    const size_t m_mask;
    std::vector<Slot> m_slots;
    WaiterQueue m_recvWaiters;
    WaiterQueue m_sendWaiters;
};



namespace detail {

/**
 * @brief 等待已被对端认领的 waiter 完成有限的值搬运和状态发布窗口。
 * @tparam T 通道元素类型。
 * @param waiter 当前状态可能为 kFulfilling 的等待体。
 * @note 仅在认领已完成后短时自旋，不等待 ring 的满/空条件；调用期间会短时占用当前线程。
 */
template <BoundedValue T>
void waitForBoundedChannelFulfillment(BoundedChannelWaiter<T>& waiter) noexcept
{
    // 对端已经通过 CAS 认领 waiter，发布只剩值搬运和一次 release store；
    // 这里不是等待条件成立的长时间自旋，而是等待该有限发布窗口完成。
    while (waiter.state.load(std::memory_order_acquire) == BoundedWaiterState::kFulfilling) {
        boundedChannelCpuPause();
    }
}

} // namespace detail

template <BoundedValue T>
inline BoundedSendAwaitable<T> BoundedChannel<T>::send(T&& value)
{
    return BoundedSendAwaitable<T>(this, std::move(value));
}

template <BoundedValue T>
inline BoundedRecvAwaitable<T> BoundedChannel<T>::recv()
{
    return BoundedRecvAwaitable<T>(this);
}

template <BoundedValue T>
inline BoundedRecvBatchAwaitable<T> BoundedChannel<T>::recvBatch(size_t count)
{
    return BoundedRecvBatchAwaitable<T>(this, count);
}

template <BoundedValue T>
inline bool BoundedSendAwaitable<T>::await_ready() noexcept
{
    if (m_channel->trySend(std::move(m_value))) {
        m_sent = true;
        return true;
    }
    return m_channel->isClosed();
}

template <BoundedValue T>
template <typename Promise>
inline bool BoundedSendAwaitable<T>::await_suspend(
    std::coroutine_handle<Promise> handle) noexcept
{
    auto* channel = m_channel;
    if (channel->trySend(std::move(m_value))) {
        m_sent = true;
        return false;
    }
    if (channel->isClosed()) {
        return false;
    }

    TimeoutTimer::ptr timeoutTimer = std::move(m_timeoutTimer);
    auto waiter = std::make_shared<BoundedChannelWaiter<T>>(
        Waker(handle), std::move(timeoutTimer));
    waiter->value.emplace(std::move(m_value));
    m_waiter = waiter;
    channel->m_sendWaiterPathUsed.store(true, std::memory_order_seq_cst);
    if (!channel->enqueueWaiter(channel->m_sendWaiters, waiter)) {
        waiter->state.store(BoundedWaiterState::kFailed, std::memory_order_release);
        return false;
    }
    channel->synchronizeSendWaiterPath();
    channel->requestPump(BoundedChannel<T>::kSendWork);
    if (waiter->timeoutTimer != nullptr) {
        // WithTimeout 在 timer 注册完成后统一发布共享 completion gate。
        return true;
    }
    // arm() 后对端可能立即恢复并销毁 awaiter/channel，因此必须直接返回。
    return waiter->completionWaker->arm();
}

template <BoundedValue T>
inline std::expected<void, IOError> BoundedSendAwaitable<T>::await_resume() noexcept
{
    if (m_sent) {
        return {};
    }
    if (m_waiter) {
        detail::waitForBoundedChannelFulfillment(*m_waiter);
        const auto state = m_waiter->state.load(std::memory_order_acquire);
        m_waiter->completionWaker->clearWaker();
        if (state == BoundedWaiterState::kFulfilled) {
            return {};
        }
        if (state == BoundedWaiterState::kClosed) {
            return std::unexpected(IOError(kClosed, 0));
        }
        if (state == BoundedWaiterState::kFailed) {
            return std::unexpected(IOError(kNotReady, 0));
        }
    }
    if (m_timedOut) {
        return std::unexpected(IOError(kTimeout, 0));
    }
    if (m_channel->isClosed()) {
        return std::unexpected(IOError(kClosed, 0));
    }
    return std::unexpected(IOError(kTimeout, 0));
}

template <BoundedValue T>
inline void BoundedSendAwaitable<T>::markTimeout() noexcept
{
    m_timedOut = true;
    if (!m_waiter) {
        return;
    }
    BoundedWaiterState expected = BoundedWaiterState::kWaiting;
    bool cancelled = false;
    if (m_waiter->state.compare_exchange_strong(expected,
                                                BoundedWaiterState::kCancelled,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
        cancelled = true;
        m_waiter->value.reset();
        m_waiter->completionWaker->clearWaker();
    } else if (expected == BoundedWaiterState::kCancelled) {
        cancelled = true;
        m_waiter->completionWaker->clearWaker();
    }
    if (cancelled) {
        m_channel->requestPump(BoundedChannel<T>::kSendWork);
    }
}

template <BoundedValue T>
inline bool BoundedRecvAwaitable<T>::await_ready() noexcept
{
    if (auto value = m_channel->tryRecv(); value.has_value()) {
        m_ready.emplace(std::move(*value));
        return true;
    }
    return m_channel->isClosedAndDrained();
}

template <BoundedValue T>
template <typename Promise>
inline bool BoundedRecvAwaitable<T>::await_suspend(
    std::coroutine_handle<Promise> handle) noexcept
{
    auto* channel = m_channel;
    if (auto value = channel->tryRecv(); value.has_value()) {
        m_ready.emplace(std::move(*value));
        return false;
    }

    TimeoutTimer::ptr timeoutTimer = std::move(m_timeoutTimer);
    auto waiter = std::make_shared<BoundedChannelWaiter<T>>(
        Waker(handle), std::move(timeoutTimer));
    m_waiter = waiter;
    channel->m_recvWaiterPathUsed.store(true, std::memory_order_seq_cst);
    if (!channel->enqueueWaiter(channel->m_recvWaiters, waiter)) {
        waiter->state.store(BoundedWaiterState::kFailed, std::memory_order_release);
        return false;
    }
    channel->synchronizeRecvWaiterPath();
    channel->requestPump(BoundedChannel<T>::kRecvWork);
    if (waiter->timeoutTimer != nullptr) {
        return true;
    }
    return waiter->completionWaker->arm();
}

template <BoundedValue T>
inline std::expected<T, IOError> BoundedRecvAwaitable<T>::await_resume() noexcept
{
    if (m_ready.has_value()) {
        return std::move(*m_ready);
    }
    if (m_waiter) {
        detail::waitForBoundedChannelFulfillment(*m_waiter);
        const auto state = m_waiter->state.load(std::memory_order_acquire);
        if (state == BoundedWaiterState::kFulfilled && m_waiter->value.has_value()) {
            T value = std::move(*m_waiter->value);
            m_waiter->value.reset();
            m_waiter->completionWaker->clearWaker();
            return value;
        }
        m_waiter->completionWaker->clearWaker();
        if (state == BoundedWaiterState::kClosed) {
            return std::unexpected(IOError(kClosed, 0));
        }
        if (state == BoundedWaiterState::kFailed) {
            return std::unexpected(IOError(kNotReady, 0));
        }
        if (state == BoundedWaiterState::kFulfilled) {
            return std::unexpected(IOError(kClosed, 0));
        }
    }
    if (m_timedOut) {
        return std::unexpected(IOError(kTimeout, 0));
    }
    if (m_channel->isClosedAndDrained()) {
        return std::unexpected(IOError(kClosed, 0));
    }
    if (auto value = m_channel->tryRecv(); value.has_value()) {
        return std::move(*value);
    }
    return std::unexpected(IOError(kTimeout, 0));
}

template <BoundedValue T>
inline void BoundedRecvAwaitable<T>::markTimeout() noexcept
{
    m_timedOut = true;
    if (!m_waiter) {
        return;
    }
    BoundedWaiterState expected = BoundedWaiterState::kWaiting;
    bool cancelled = false;
    if (m_waiter->state.compare_exchange_strong(expected,
                                                BoundedWaiterState::kCancelled,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
        cancelled = true;
        m_waiter->completionWaker->clearWaker();
    } else if (expected == BoundedWaiterState::kCancelled) {
        cancelled = true;
        m_waiter->completionWaker->clearWaker();
    }
    if (cancelled) {
        m_channel->requestPump(BoundedChannel<T>::kRecvWork);
    }
}

template <BoundedValue T>
inline bool BoundedRecvBatchAwaitable<T>::await_ready() noexcept
{
    if (auto values = m_channel->tryRecvBatch(m_count); values.has_value()) {
        m_ready.emplace(std::move(*values));
        return true;
    }
    return m_channel->isClosedAndDrained();
}

template <BoundedValue T>
template <typename Promise>
inline bool BoundedRecvBatchAwaitable<T>::await_suspend(
    std::coroutine_handle<Promise> handle) noexcept
{
    auto* channel = m_channel;
    if (auto values = channel->tryRecvBatch(m_count); values.has_value()) {
        m_ready.emplace(std::move(*values));
        return false;
    }

    TimeoutTimer::ptr timeoutTimer = std::move(m_timeoutTimer);
    auto waiter = std::make_shared<BoundedChannelWaiter<T>>(
        Waker(handle), std::move(timeoutTimer));
    m_waiter = waiter;
    channel->m_recvWaiterPathUsed.store(true, std::memory_order_seq_cst);
    if (!channel->enqueueWaiter(channel->m_recvWaiters, waiter)) {
        waiter->state.store(BoundedWaiterState::kFailed, std::memory_order_release);
        return false;
    }
    channel->synchronizeRecvWaiterPath();
    channel->requestPump(BoundedChannel<T>::kRecvWork);
    if (waiter->timeoutTimer != nullptr) {
        return true;
    }
    return waiter->completionWaker->arm();
}

template <BoundedValue T>
inline std::expected<std::vector<T>, IOError> BoundedRecvBatchAwaitable<T>::await_resume() noexcept
{
    if (m_ready.has_value()) {
        return std::move(*m_ready);
    }
    if (m_waiter) {
        detail::waitForBoundedChannelFulfillment(*m_waiter);
        const auto state = m_waiter->state.load(std::memory_order_acquire);
        if (state == BoundedWaiterState::kFulfilled && m_waiter->value.has_value()) {
            std::vector<T> values;
            values.reserve(m_count);
            values.push_back(std::move(*m_waiter->value));
            m_waiter->value.reset();
            while (values.size() < m_count) {
                auto value = m_channel->tryRecv();
                if (!value.has_value()) {
                    break;
                }
                values.push_back(std::move(*value));
            }
            m_waiter->completionWaker->clearWaker();
            return values;
        }
        m_waiter->completionWaker->clearWaker();
        if (state == BoundedWaiterState::kClosed || state == BoundedWaiterState::kFulfilled) {
            return std::unexpected(IOError(kClosed, 0));
        }
        if (state == BoundedWaiterState::kFailed) {
            return std::unexpected(IOError(kNotReady, 0));
        }
    }
    if (m_timedOut) {
        return std::unexpected(IOError(kTimeout, 0));
    }
    if (m_channel->isClosedAndDrained()) {
        return std::unexpected(IOError(kClosed, 0));
    }
    if (auto values = m_channel->tryRecvBatch(m_count); values.has_value()) {
        return std::move(*values);
    }
    return std::unexpected(IOError(kTimeout, 0));
}

template <BoundedValue T>
inline void BoundedRecvBatchAwaitable<T>::markTimeout() noexcept
{
    m_timedOut = true;
    if (!m_waiter) {
        return;
    }
    BoundedWaiterState expected = BoundedWaiterState::kWaiting;
    bool cancelled = false;
    if (m_waiter->state.compare_exchange_strong(expected,
                                                BoundedWaiterState::kCancelled,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
        cancelled = true;
        m_waiter->completionWaker->clearWaker();
    } else if (expected == BoundedWaiterState::kCancelled) {
        cancelled = true;
        m_waiter->completionWaker->clearWaker();
    }
    if (cancelled) {
        m_channel->requestPump(BoundedChannel<T>::kRecvWork);
    }
}


} // namespace galay::mpmc

#endif // GALAY_CONCURRENCY_MPMC_BOUNDED_CHANNEL_H
