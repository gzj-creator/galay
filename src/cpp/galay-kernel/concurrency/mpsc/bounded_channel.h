/**
 * @file bounded_channel.h
 * @brief 有界 MPSC 异步通道。
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 通道使用预分配的环形队列作为数据通路。多个生产者通过 tail CAS
 * 认领 slot，唯一消费者直接推进 head；满或空时，异步操作只挂起协程，
 * 不阻塞调度器线程。
 */

#ifndef GALAY_CONCURRENCY_MPSC_BOUNDED_CHANNEL_H
#define GALAY_CONCURRENCY_MPSC_BOUNDED_CHANNEL_H

#include "../../common/error.h"
#include "../../core/timeout.hpp"
#include "../../core/waker.h"
#include <coroutine>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <emmintrin.h>
#endif

#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace galay::mpsc
{

using kernel::IOError;
using kernel::TimeoutTimer;
using kernel::TimeoutSupport;
using kernel::Waker;
using kernel::WithTimeout;
using kernel::kClosed;
using kernel::kNotReady;
using kernel::kTimeout;

/**
 * @brief 约束 BoundedChannel 可存储的元素类型。
 * @tparam T 元素类型；必须可移动且移动构造不得抛出异常。
 */
template <typename T>
concept BoundedValue =
    std::movable<T> && std::is_nothrow_move_constructible_v<T>;

struct BoundedChannelTestAccess;

template <BoundedValue T>
class BoundedChannel;

template <BoundedValue T>
class BoundedSendAwaitable;

template <BoundedValue T>
class BoundedRecvAwaitable;

template <BoundedValue T>
class BoundedRecvBatchAwaitable;

namespace bounded_detail
{

/**
 * @brief 执行一次平台相关的短时 CPU 自旋提示。
 * @note 该函数不阻塞线程，也不主动把执行权交给操作系统调度器。
 */
inline void cpuPause() noexcept
{
#if defined(_MSC_VER)
    YieldProcessor();
#elif defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__APPLE__) && defined(__aarch64__)
    // Apple silicon 高竞争压测中 YIELD hint 会过度让步，使用 ISB 保持短自旋。
    __asm__ __volatile__("isb" ::: "memory");
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#else
    // 未提供平台 pause 指令时，退避窗口仍然只有数条指令。
#endif
}

/**
 * @brief 多生产者竞争 tail 以及等待 slot 发布时使用的有界退避。
 */
class ProducerBackoff
{
public:
    /** @brief 对 tail CAS 竞争执行有上限的指数自旋。 */
    void spin() noexcept
    {
        const uint32_t step = m_step < kSpinLimit ? m_step : kSpinLimit;
        const uint32_t spins = 1U << step;
        for (uint32_t i = 0; i < spins; ++i) {
            cpuPause();
        }
        if (m_step <= kSpinLimit) {
            ++m_step;
        }
    }

    /** @brief 等待 slot sequence 更新；持续等待后才让出时间片。 */
    void snooze() noexcept
    {
        if (m_step <= kSpinLimit) {
            const uint32_t spins = 1U << m_step;
            for (uint32_t i = 0; i < spins; ++i) {
                cpuPause();
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

enum class WaiterState : uint8_t {
    kWaiting,       ///< 已注册但尚未被认领
    kCancelled,     ///< 已由超时路径取消
    kFulfilling,    ///< 对端已认领，正在搬运值
    kFulfilled,     ///< 已成功交接
    kClosed,        ///< 因通道关闭结束
    kFailed         ///< 等待队列无法接收 waiter，返回 kNotReady
};

enum class WaiterProgress : uint8_t {
    kNotClaimed, ///< waiter 已被其他路径认领或参数无效
    kWaiting,    ///< waiter 尚未完成并已重新进入等待队列
    kCompleted,  ///< waiter 已成功、关闭或失败结束，当前资源事件已处理
    kSkipped     ///< waiter 已结束但未消费当前资源，scanner 必须继续
};

enum class TestHookPoint : uint8_t {
    kSendBeforeArm,
    kRecvBeforeArm,
    kPumpBeforeRelease
};

#if defined(GALAY_MPSC_BOUNDED_TEST_HOOKS)
using TestHook = void (*)(TestHookPoint, void*) noexcept;

inline std::atomic<TestHook> g_testHook{nullptr};
inline std::atomic<void*> g_testHookContext{nullptr};

inline void setTestHook(TestHook hook, void* context) noexcept
{
    g_testHookContext.store(context, std::memory_order_release);
    g_testHook.store(hook, std::memory_order_release);
}

inline void clearTestHook() noexcept
{
    g_testHook.store(nullptr, std::memory_order_release);
    g_testHookContext.store(nullptr, std::memory_order_release);
}

inline void invokeTestHook(TestHookPoint point) noexcept
{
    const TestHook hook = g_testHook.load(std::memory_order_acquire);
    if (hook != nullptr) {
        hook(point, g_testHookContext.load(std::memory_order_acquire));
    }
}
#else
inline void invokeTestHook(TestHookPoint) noexcept {}
#endif

/**
 * @brief BoundedChannel 的异步等待体。
 * @tparam T 通道元素类型。
 * @details 接收者使用 value 保存交接到的消息；发送者使用 value 保存待发送消息。
 */
template <BoundedValue T>
struct ChannelWaiter
{
    std::optional<T> value;
    std::shared_ptr<ChannelWaiter<T>> deferredWakeNext;
    kernel::detail::DeferredWaker localWaker;
    const TimeoutTimer::ptr timeoutTimer;
    kernel::detail::DeferredWaker* const completionWaker;
    std::atomic<WaiterState> state{WaiterState::kWaiting};
    std::atomic<bool> queued{false};
    bool deferredTimeoutWake{false};

    /**
     * @brief 创建绑定指定协程唤醒器的等待体。
     * @param waiterWaker 等待操作完成时使用的唤醒器，所有权移入 waiter。
     * @param waiterTimeoutTimer 可选的 timeout 完成权；发布后只读并与 waiter 共存活。
     */
    explicit ChannelWaiter(Waker waiterWaker,
                           TimeoutTimer::ptr waiterTimeoutTimer = {})
        : localWaker(waiterTimeoutTimer ? Waker() : std::move(waiterWaker))
        , timeoutTimer(std::move(waiterTimeoutTimer))
        , completionWaker(timeoutTimer != nullptr
              ? &timeoutTimer->completionWaker()
              : &localWaker)
    {
    }
};

/**
 * @brief 等待已被对端认领的 waiter 完成有限的值搬运和状态发布窗口。
 * @tparam T 通道元素类型。
 * @param waiter 当前状态可能为 kFulfilling 的等待体。
 * @note 这里只等待一次有限发布窗口，不等待 ring 的满或空条件。
 */
template <BoundedValue T>
void waitForFulfillment(ChannelWaiter<T>& waiter) noexcept
{
    while (waiter.state.load(std::memory_order_acquire) == WaiterState::kFulfilling) {
        cpuPause();
    }
}

} // namespace bounded_detail

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

    /** @brief 把 WithTimeout 的完成权绑定到后续发布的 channel waiter。 */
    void bindTimeoutTimer(const TimeoutTimer::ptr& timer) noexcept
    {
        m_timeoutTimer = timer;
    }

    BoundedChannel<T>* m_channel;
    T m_value;
    std::shared_ptr<bounded_detail::ChannelWaiter<T>> m_waiter;
    TimeoutTimer::ptr m_timeoutTimer;
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

    /** @brief 把 WithTimeout 的完成权绑定到后续发布的 channel waiter。 */
    void bindTimeoutTimer(const TimeoutTimer::ptr& timer) noexcept
    {
        m_timeoutTimer = timer;
    }

    BoundedChannel<T>* m_channel;
    std::optional<T> m_ready;
    std::shared_ptr<bounded_detail::ChannelWaiter<T>> m_waiter;
    TimeoutTimer::ptr m_timeoutTimer;
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

    /** @brief 把 WithTimeout 的完成权绑定到后续发布的 channel waiter。 */
    void bindTimeoutTimer(const TimeoutTimer::ptr& timer) noexcept
    {
        m_timeoutTimer = timer;
    }

    BoundedChannel<T>* m_channel;
    size_t m_count;
    std::optional<std::vector<T>> m_ready;
    std::shared_ptr<bounded_detail::ChannelWaiter<T>> m_waiter;
    TimeoutTimer::ptr m_timeoutTimer;
    bool m_timedOut = false;
};

/**
 * @brief 固定容量的多生产者单消费者异步通道。
 * @tparam T 元素类型，要求可移动且移动构造不得抛出异常。
 *
 * @details
 * - 容量会向上取整为不小于 2 的 2 的幂，超出安全上限时钳制到该上限。
 * - 多个生产者通过 tail CAS 并发认领 slot；每个 slot 的 sequence 发布消息并控制复用。
 * - 唯一消费者直接推进 head，不执行消费者间 CAS 或竞争退避。
 * - send() / recv() / recvBatch() 满或空时只挂起协程，不阻塞底层线程。
 * - 多生产者之间不保证全局顺序；每个生产者自身发送的消息保持 FIFO。
 *
 * @note 调用方必须保证同一时刻只有一个消费者执行接收操作。生产者可以代替挂起的
 *       接收协程完成一次 ring 消费；waiter 状态 CAS 保证该逻辑消费者只由一个线程推进。
 * @note 通道是身份对象，不可复制或移动。通道必须存活到所有挂起操作完成或超时。
 * @note trySend(T&&) 只有在成功抢到 slot 后才移动参数；失败时参数保持未移动。
 */
template <BoundedValue T>
class BoundedChannel
{
public:
    static_assert(BoundedValue<T>,
                  "BoundedChannel requires a nothrow-move-constructible T");

    /**
     * @brief 构造有界 MPSC 通道。
     * @param capacity 期望容量；小值提升到 2，其余向上取整并钳制到安全上限。
     * @note 构造完成后可由多个生产者和唯一消费者并发访问。
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
        const size_t head = m_head.load(std::memory_order_relaxed);
        const size_t tail =
            m_tail.load(std::memory_order_relaxed) & kTailPositionMask;
        const size_t pending = tail - head;
        const size_t count = pending < m_capacity ? pending : m_capacity;
        for (size_t i = 0; i < count; ++i) {
            const size_t position = head + i;
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
     * @note 多个生产者可并发调用；tail CAS 竞争在当前调用内通过 CPU hint 重试。
     */
    bool trySend(T&& value)
    {
        const auto result = ringEnqueueResult(std::move(value));
        if (result == RingEnqueueResult::kClosedAndNotify) {
            requestPump(static_cast<uint8_t>(kRecvWork | kSendWork));
        }
        if (result != RingEnqueueResult::kSent) {
            return false;
        }
        if (m_recvWaiterCount.load(std::memory_order_seq_cst) != 0) {
            requestPump(kRecvWork);
        }
        return true;
    }

    /**
     * @brief 复制并尝试立即发送一条消息。
     * @param value 待复制消息。
     * @return true 表示发送成功；false 表示通道已关闭或当前已满。
     * @note 先创建本地副本，再复用移动发送路径；发送失败不会修改原值。
     */
    bool trySend(const T& value) requires std::copy_constructible<T>
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
     * @note 只能由唯一消费者调用；head 通过原子 load/store 推进，不执行消费者 CAS。
     */
    std::optional<T> tryRecv()
    {
        std::optional<T> value;
        if (ringDequeueTo([&value](T&& item) {
                value.emplace(std::move(item));
            })) {
            if (m_sendWaiterCount.load(std::memory_order_seq_cst) != 0) {
                requestPump(kSendWork);
            }
            return value;
        }
        return std::nullopt;
    }

    /**
     * @brief 异步接收一条消息。
     * @return 可 co_await 的等待体；关闭且已排空时返回 IOError(kClosed, 0)，超时返回 IOError(kTimeout, 0)。
     * @note 空时挂起协程而不阻塞调度器线程；调用方必须遵守单消费者约束。
     */
    BoundedRecvAwaitable<T> recv();

    /**
     * @brief 异步批量接收消息。
     * @param count 单次最多接收的消息数。
     * @return 可 co_await 的等待体；至少收到一条后尽量补齐至 count 条。
     * @note count 为 0 时立即返回空批次；调用方必须遵守单消费者约束。
     */
    BoundedRecvBatchAwaitable<T> recvBatch(size_t count);

    /**
     * @brief 尝试批量接收消息。
     * @param count 单次最多接收的消息数；0 返回空 vector。
     * @return 收到至少一条消息时返回消息批次；否则返回 std::nullopt。
     * @note 唯一消费者使用本地 head 连续排空，不会等待后续消息来补齐 count。
     */
    std::optional<std::vector<T>> tryRecvBatch(size_t count)
    {
        if (count == 0) {
            return std::vector<T>{};
        }

        std::vector<T> values;
        values.reserve(count);
        size_t position = m_head.load(std::memory_order_relaxed);
        while (values.size() < count) {
            Slot& slot = m_slots[position & m_mask];
            const size_t sequence =
                slot.sequence.load(std::memory_order_acquire);
            if (sequence != position + 1) {
                break;
            }
            values.push_back(std::move(*slot.value()));
            std::destroy_at(slot.value());
            slot.sequence.store(position + m_capacity,
                                std::memory_order_seq_cst);
            ++position;
        }
        if (values.empty()) {
            return std::nullopt;
        }
        m_head.store(position, std::memory_order_release);
        if (m_sendWaiterCount.load(std::memory_order_seq_cst) != 0) {
            requestPump(kSendWork);
        }
        return values;
    }

    /**
     * @brief 关闭通道并唤醒所有等待者。
     * @details 关闭状态通过原子变量发布，可与发送、接收及其他 close() 调用并发执行。
     * @note 操作幂等；关闭后发送失败，接收仍会先排空 ring 中的残留消息。
     */
    void close() noexcept
    {
        const size_t previous =
            m_tail.fetch_or(kTailClosedBit, std::memory_order_acq_rel);
        if ((previous & kTailClosedBit) != 0) {
            return;
        }
        requestPump(static_cast<uint8_t>(kRecvWork | kSendWork));
    }

    /**
     * @brief 查询通道是否已关闭。
     * @return true 表示已关闭。
     */
    bool isClosed() const noexcept
    {
        return (m_tail.load(std::memory_order_acquire) & kTailClosedBit) != 0;
    }

    /**
     * @brief 返回实际生效容量。
     * @return 取整并钳制后的 2 的幂容量。
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
        const size_t tail =
            m_tail.load(std::memory_order_relaxed) & kTailPositionMask;
        const size_t head = m_head.load(std::memory_order_relaxed);
        if (tail < head) {
            return 0;
        }
        const size_t count = tail - head;
        return count < m_capacity ? count : m_capacity;
    }

    /** @brief 近似检查 ring 是否为空；结果不可用作同步条件。 */
    bool empty() const noexcept
    {
        return size() == 0;
    }

    /** @brief 近似检查 ring 是否已满；结果不可用作同步条件。 */
    bool full() const noexcept
    {
        return size() >= m_capacity;
    }

private:
    // Apple silicon 的一致性粒度是 128B，x86 是 64B；固定值保证公开模板布局稳定。
#if defined(__aarch64__) || defined(__ARM_ARCH_ISA_A64)
    static constexpr size_t kCacheLine = 128;
#else
    static constexpr size_t kCacheLine = 64;
#endif
    static constexpr size_t kTailClosedBit =
        size_t{1} << (sizeof(size_t) * 8U - 1U);
    static constexpr size_t kTailPositionMask = kTailClosedBit - 1U;
    static constexpr size_t kMaxRingCapacity = kTailClosedBit >> 1U;

    struct Slot
    {
        alignas(T) std::byte storage[sizeof(T)];
        std::atomic<size_t> sequence{0};
        // storage 在 sequence 前，避免高对齐 T 被小成员打断。

        /**
         * @brief 获取尚未开始 T 生命周期的原始存储地址。
         * @return 仅供 std::construct_at 使用的目标地址，不得解引用。
         */
        T* rawStorage() noexcept
        {
            return reinterpret_cast<T*>(storage);
        }

        /** @brief 获取已构造对象的 T 指针。 */
        T* value() noexcept
        {
            return std::launder(rawStorage());
        }
    };

    using Waiter = bounded_detail::ChannelWaiter<T>;
    using WaiterPtr = std::shared_ptr<Waiter>;

    static bool tryIncrementWaiterCount(
        std::atomic<size_t>& activeCount) noexcept
    {
        size_t current = activeCount.load(std::memory_order_seq_cst);
        while (current != std::numeric_limits<size_t>::max()) {
            if (activeCount.compare_exchange_weak(current,
                                                  current + 1,
                                                  std::memory_order_seq_cst,
                                                  std::memory_order_seq_cst)) {
                return true;
            }
        }
        return false;
    }

    static void decrementWaiterCount(
        std::atomic<size_t>& activeCount) noexcept
    {
        size_t current = activeCount.load(std::memory_order_seq_cst);
        while (current != 0) {
            if (activeCount.compare_exchange_weak(current,
                                                  current - 1,
                                                  std::memory_order_seq_cst,
                                                  std::memory_order_seq_cst)) {
                return;
            }
        }
    }

    /**
     * @brief 多生产者入队、唯一 pump 出队的非阻塞 waiter 队列。
     * @details 每次慢路径注册分配一个 linked node。producer 先交换 head，再以
     *          release 发布 predecessor->next；consumer 只由 pump owner 执行，
     *          因此不需要 dequeue CAS 或 hazard pointer，旧 dummy 可安全删除。
     */
    class WaiterQueue
    {
    private:
        struct Node
        {
            WaiterPtr waiter;
            std::atomic<Node*> next{nullptr};

            Node() noexcept = default;
            explicit Node(const WaiterPtr& input) noexcept : waiter(input) {}
        };

    public:
        WaiterQueue() noexcept
            : m_producerHead(&m_stub), m_consumerTail(&m_stub)
        {
        }

        WaiterQueue(const WaiterQueue&) = delete;
        WaiterQueue& operator=(const WaiterQueue&) = delete;
        WaiterQueue(WaiterQueue&&) = delete;
        WaiterQueue& operator=(WaiterQueue&&) = delete;

        ~WaiterQueue() noexcept
        {
            WaiterPtr ignored;
            while (tryDequeue(ignored)) {
            }
            if (m_consumerTail != &m_stub) {
                delete m_consumerTail;
            }
        }

        bool enqueue(const WaiterPtr& waiter,
                     std::atomic<size_t>& activeCount) noexcept
        {
            Node* const node = new (std::nothrow) Node(waiter);
            if (node == nullptr) {
                return false;
            }
            // 先计数再发布节点，pump 不会观察到尚未计数的有效入口。
            if (!tryIncrementWaiterCount(activeCount)) {
                delete node;
                return false;
            }
            Node* const previous =
                m_producerHead.exchange(node, std::memory_order_acq_rel);
            previous->next.store(node, std::memory_order_release);
            return true;
        }

        bool tryDequeue(WaiterPtr& waiter) noexcept
        {
            Node* const tail = m_consumerTail;
            Node* const next = tail->next.load(std::memory_order_acquire);
            if (next == nullptr) {
                return false;
            }
            waiter = std::move(next->waiter);
            m_consumerTail = next;
            if (tail != &m_stub) {
                delete tail;
            }
            return true;
        }

    private:
        alignas(kCacheLine) std::atomic<Node*> m_producerHead;
        alignas(kCacheLine) Node* m_consumerTail;
        Node m_stub;
    };

    static constexpr uint8_t kPumpRunning = 0x01;
    static constexpr uint8_t kRecvWork = 0x02;
    static constexpr uint8_t kSendWork = 0x04;

    enum class RingEnqueueResult : uint8_t {
        kSent,
        kFull,
        kClosed,
        kClosedAndNotify,
    };

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

    /** @note 任一唤醒都可能 inline 销毁 channel，本函数不得访问 this。 */
    static void issueDeferredWakes(WaiterPtr wakeHead) noexcept
    {
        while (wakeHead) {
            WaiterPtr next = std::move(wakeHead->deferredWakeNext);
            const bool timeoutWinner = wakeHead->deferredTimeoutWake;
            wakeHead->deferredTimeoutWake = false;
            if (timeoutWinner && wakeHead->timeoutTimer != nullptr) {
                wakeHead->timeoutTimer->wakeTimeoutWinner();
            } else if (!wakeHead->completionWaker->requestWake()) {
                // timeout 或其他完成路径已发布同一 waiter 的唤醒。
            }
            wakeHead = std::move(next);
        }
    }

    /** @brief 尝试由唯一 pump owner 认领等待中的 waiter。 */
    bool claimWaiter(const WaiterPtr& waiter) noexcept
    {
        bounded_detail::WaiterState expected = bounded_detail::WaiterState::kWaiting;
        return waiter->state.compare_exchange_strong(
            expected,
            bounded_detail::WaiterState::kFulfilling,
            std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    bool enqueueWaiter(WaiterQueue& waiters,
                       std::atomic<size_t>& activeCount,
                       const WaiterPtr& waiter) noexcept
    {
        bool expected = false;
        if (!waiter->queued.compare_exchange_strong(expected,
                                                    true,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_acquire)) {
            return true;
        }
        if (waiters.enqueue(waiter, activeCount)) {
            return true;
        }
        waiter->queued.store(false, std::memory_order_release);
        return false;
    }

    bool tryDequeueWaiter(WaiterQueue& waiters, WaiterPtr& waiter) noexcept
    {
        while (waiters.tryDequeue(waiter)) {
            if (waiter && waiter->queued.exchange(false, std::memory_order_acq_rel)) {
                return true;
            }
        }
        return false;
    }

    void synchronizeRecvWaiterPath() noexcept
    {
        const size_t position = m_head.load(std::memory_order_relaxed);
        [[maybe_unused]] const size_t published =
            m_slots[position & m_mask].sequence.load(std::memory_order_seq_cst);
    }

    void synchronizeSendWaiterPath() noexcept
    {
        const size_t tail = m_tail.load(std::memory_order_relaxed);
        const size_t position = tail & kTailPositionMask;
        [[maybe_unused]] const size_t released =
            m_slots[position & m_mask].sequence.load(std::memory_order_seq_cst);
    }

    static size_t normalizeCapacity(size_t capacity) noexcept
    {
        if (capacity <= 2) {
            return 2;
        }
        // sequence 差值按有符号半区间解释；同时避免 bit_ceil 在最高位以上溢出。
        if (capacity >= kMaxRingCapacity) {
            return kMaxRingCapacity;
        }
        return std::bit_ceil(capacity);
    }

    RingEnqueueResult ringEnqueueResult(T&& value) noexcept
    {
        size_t tail = m_tail.load(std::memory_order_relaxed);
        Slot* slot = nullptr;
        size_t position = 0;
        bounded_detail::ProducerBackoff backoff;
        for (;;) {
            if ((tail & kTailClosedBit) != 0) {
                return RingEnqueueResult::kClosed;
            }
            position = tail;
            if (position == kTailPositionMask) {
                size_t expected = position;
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
            const size_t sequence = slot->sequence.load(std::memory_order_acquire);
            using SignedSize = std::make_signed_t<size_t>;
            const SignedSize difference =
                std::bit_cast<SignedSize>(sequence - position);
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
            std::construct_at(slot->rawStorage(), std::move(value));
        slot->sequence.store(position + 1, std::memory_order_seq_cst);
        return RingEnqueueResult::kSent;
    }

    /** @brief 白盒测试兼容入口；公开快路径使用 ringEnqueueResult() 区分关闭。 */
    bool ringEnqueue(T&& value) noexcept
    {
        const auto result = ringEnqueueResult(std::move(value));
        if (result == RingEnqueueResult::kClosedAndNotify) {
            requestPump(static_cast<uint8_t>(kRecvWork | kSendWork));
        }
        return result == RingEnqueueResult::kSent;
    }

    bool isClosedAndDrained() const noexcept
    {
        const size_t tail = m_tail.load(std::memory_order_acquire);
        if ((tail & kTailClosedBit) == 0) {
            return false;
        }
        return (tail & kTailPositionMask) ==
            m_head.load(std::memory_order_acquire);
    }

    template <typename Consume>
    bool ringDequeueTo(Consume&& consume) noexcept
    {
        const size_t position = m_head.load(std::memory_order_relaxed);
        Slot& slot = m_slots[position & m_mask];
        const size_t sequence = slot.sequence.load(std::memory_order_acquire);
        if (sequence != position + 1) {
            return false;
        }

        consume(std::move(*slot.value()));
        std::destroy_at(slot.value());
        slot.sequence.store(position + m_capacity, std::memory_order_seq_cst);
        m_head.store(position + 1, std::memory_order_release);
        return true;
    }

    bounded_detail::WaiterProgress tryCompleteSendWaiter(
        const WaiterPtr& waiter,
        WaiterPtr& wakeHead,
        WaiterPtr& wakeTail) noexcept
    {
        if (!waiter) {
            return bounded_detail::WaiterProgress::kSkipped;
        }

        TimeoutTimer* const timeoutTimer = waiter->timeoutTimer.get();
        bool timeoutOperationStarted = false;
        if (timeoutTimer != nullptr) {
            const auto start = timeoutTimer->tryBeginOperation();
            if (start == TimeoutTimer::OperationStart::kBusy) {
                if (waiter->state.load(std::memory_order_acquire) ==
                        bounded_detail::WaiterState::kWaiting &&
                    enqueueWaiter(m_sendWaiters,
                                  m_sendWaiterCount,
                                  waiter)) {
                    return bounded_detail::WaiterProgress::kWaiting;
                }
                return bounded_detail::WaiterProgress::kSkipped;
            }
            if (start == TimeoutTimer::OperationStart::kTimeoutWon) {
                bounded_detail::WaiterState expected =
                    bounded_detail::WaiterState::kWaiting;
                if (waiter->state.compare_exchange_strong(
                        expected,
                        bounded_detail::WaiterState::kCancelled,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    waiter->value.reset();
                }
                return bounded_detail::WaiterProgress::kSkipped;
            }
            if (start == TimeoutTimer::OperationStart::kOperationWon) {
                return bounded_detail::WaiterProgress::kSkipped;
            }
            timeoutOperationStarted = true;
        }

        if (!claimWaiter(waiter)) {
            if (timeoutOperationStarted) {
                const auto aborted = timeoutTimer->abortOperation();
                if (aborted == TimeoutTimer::OperationAbort::kTimeoutWon) {
                    bounded_detail::WaiterState expected =
                        bounded_detail::WaiterState::kWaiting;
                    if (waiter->state.compare_exchange_strong(
                            expected,
                            bounded_detail::WaiterState::kCancelled,
                            std::memory_order_acq_rel,
                            std::memory_order_acquire)) {
                        waiter->value.reset();
                    }
                    appendDeferredWake(wakeHead, wakeTail, waiter, true);
                }
            }
            return bounded_detail::WaiterProgress::kSkipped;
        }

        if (!waiter->value.has_value()) {
            const bool committed = !timeoutOperationStarted ||
                timeoutTimer->commitOperation();
            waiter->state.store(
                committed ? bounded_detail::WaiterState::kFailed
                          : bounded_detail::WaiterState::kCancelled,
                std::memory_order_release);
            if (committed) {
                appendDeferredWake(wakeHead, wakeTail, waiter, false);
            }
            return bounded_detail::WaiterProgress::kCompleted;
        }

        const auto result = ringEnqueueResult(std::move(*waiter->value));
        if (result == RingEnqueueResult::kSent) {
            waiter->value.reset();
            const bool committed = !timeoutOperationStarted ||
                timeoutTimer->commitOperation();
            waiter->state.store(
                committed ? bounded_detail::WaiterState::kFulfilled
                          : bounded_detail::WaiterState::kCancelled,
                std::memory_order_release);
            if (committed) {
                appendDeferredWake(wakeHead, wakeTail, waiter, false);
                if (m_recvWaiterCount.load(std::memory_order_seq_cst) != 0) {
                    requestPump(kRecvWork);
                }
            }
            return bounded_detail::WaiterProgress::kCompleted;
        }
        if (result == RingEnqueueResult::kClosed ||
            result == RingEnqueueResult::kClosedAndNotify) {
            if (result == RingEnqueueResult::kClosedAndNotify) {
                requestPump(static_cast<uint8_t>(kRecvWork | kSendWork));
            }
            waiter->value.reset();
            const bool committed = !timeoutOperationStarted ||
                timeoutTimer->commitOperation();
            waiter->state.store(
                committed ? bounded_detail::WaiterState::kClosed
                          : bounded_detail::WaiterState::kCancelled,
                std::memory_order_release);
            if (committed) {
                appendDeferredWake(wakeHead, wakeTail, waiter, false);
            }
            return bounded_detail::WaiterProgress::kCompleted;
        }

        if (!enqueueWaiter(m_sendWaiters, m_sendWaiterCount, waiter)) {
            const bool committed = !timeoutOperationStarted ||
                timeoutTimer->commitOperation();
            waiter->state.store(
                committed ? bounded_detail::WaiterState::kFailed
                          : bounded_detail::WaiterState::kCancelled,
                std::memory_order_release);
            if (committed) {
                appendDeferredWake(wakeHead, wakeTail, waiter, false);
            }
            return bounded_detail::WaiterProgress::kCompleted;
        }
        waiter->state.store(bounded_detail::WaiterState::kWaiting,
                            std::memory_order_release);
        if (!timeoutOperationStarted) {
            return bounded_detail::WaiterProgress::kWaiting;
        }

        const auto aborted = timeoutTimer->abortOperation();
        if (aborted == TimeoutTimer::OperationAbort::kRearmed) {
            return bounded_detail::WaiterProgress::kWaiting;
        }
        if (aborted == TimeoutTimer::OperationAbort::kTimeoutWon) {
            bounded_detail::WaiterState expected =
                bounded_detail::WaiterState::kWaiting;
            if (waiter->state.compare_exchange_strong(
                    expected,
                    bounded_detail::WaiterState::kCancelled,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                waiter->value.reset();
            }
            appendDeferredWake(wakeHead, wakeTail, waiter, true);
        }
        return bounded_detail::WaiterProgress::kSkipped;
    }

    bounded_detail::WaiterProgress tryCompleteRecvWaiter(
        const WaiterPtr& waiter,
        WaiterPtr& wakeHead,
        WaiterPtr& wakeTail) noexcept
    {
        if (!waiter) {
            return bounded_detail::WaiterProgress::kSkipped;
        }

        TimeoutTimer* const timeoutTimer = waiter->timeoutTimer.get();
        bool timeoutOperationStarted = false;
        if (timeoutTimer != nullptr) {
            const auto start = timeoutTimer->tryBeginOperation();
            if (start == TimeoutTimer::OperationStart::kBusy) {
                if (waiter->state.load(std::memory_order_acquire) ==
                        bounded_detail::WaiterState::kWaiting &&
                    enqueueWaiter(m_recvWaiters,
                                  m_recvWaiterCount,
                                  waiter)) {
                    return bounded_detail::WaiterProgress::kWaiting;
                }
                return bounded_detail::WaiterProgress::kSkipped;
            }
            if (start == TimeoutTimer::OperationStart::kTimeoutWon) {
                bounded_detail::WaiterState expected =
                    bounded_detail::WaiterState::kWaiting;
                [[maybe_unused]] const bool cancelled =
                    waiter->state.compare_exchange_strong(
                        expected,
                        bounded_detail::WaiterState::kCancelled,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire);
                return bounded_detail::WaiterProgress::kSkipped;
            }
            if (start == TimeoutTimer::OperationStart::kOperationWon) {
                return bounded_detail::WaiterProgress::kSkipped;
            }
            timeoutOperationStarted = true;
        }

        if (!claimWaiter(waiter)) {
            if (timeoutOperationStarted) {
                const auto aborted = timeoutTimer->abortOperation();
                if (aborted == TimeoutTimer::OperationAbort::kTimeoutWon) {
                    bounded_detail::WaiterState expected =
                        bounded_detail::WaiterState::kWaiting;
                    [[maybe_unused]] const bool cancelled =
                        waiter->state.compare_exchange_strong(
                            expected,
                            bounded_detail::WaiterState::kCancelled,
                            std::memory_order_acq_rel,
                            std::memory_order_acquire);
                    appendDeferredWake(wakeHead, wakeTail, waiter, true);
                }
            }
            return bounded_detail::WaiterProgress::kSkipped;
        }

        if (ringDequeueTo([&waiter](T&& value) {
                waiter->value.emplace(std::move(value));
            })) {
            const bool committed = !timeoutOperationStarted ||
                timeoutTimer->commitOperation();
            waiter->state.store(
                committed ? bounded_detail::WaiterState::kFulfilled
                          : bounded_detail::WaiterState::kCancelled,
                std::memory_order_release);
            if (committed) {
                appendDeferredWake(wakeHead, wakeTail, waiter, false);
                if (m_sendWaiterCount.load(std::memory_order_seq_cst) != 0) {
                    requestPump(kSendWork);
                }
            }
            return bounded_detail::WaiterProgress::kCompleted;
        }

        if (isClosedAndDrained()) {
            const bool committed = !timeoutOperationStarted ||
                timeoutTimer->commitOperation();
            waiter->state.store(
                committed ? bounded_detail::WaiterState::kClosed
                          : bounded_detail::WaiterState::kCancelled,
                std::memory_order_release);
            if (committed) {
                appendDeferredWake(wakeHead, wakeTail, waiter, false);
            }
            return bounded_detail::WaiterProgress::kCompleted;
        }

        if (!enqueueWaiter(m_recvWaiters, m_recvWaiterCount, waiter)) {
            const bool committed = !timeoutOperationStarted ||
                timeoutTimer->commitOperation();
            waiter->state.store(
                committed ? bounded_detail::WaiterState::kFailed
                          : bounded_detail::WaiterState::kCancelled,
                std::memory_order_release);
            if (committed) {
                appendDeferredWake(wakeHead, wakeTail, waiter, false);
            }
            return bounded_detail::WaiterProgress::kCompleted;
        }
        waiter->state.store(bounded_detail::WaiterState::kWaiting,
                            std::memory_order_release);
        if (!timeoutOperationStarted) {
            return bounded_detail::WaiterProgress::kWaiting;
        }

        const auto aborted = timeoutTimer->abortOperation();
        if (aborted == TimeoutTimer::OperationAbort::kRearmed) {
            return bounded_detail::WaiterProgress::kWaiting;
        }
        if (aborted == TimeoutTimer::OperationAbort::kTimeoutWon) {
            bounded_detail::WaiterState expected =
                bounded_detail::WaiterState::kWaiting;
            [[maybe_unused]] const bool cancelled =
                waiter->state.compare_exchange_strong(
                    expected,
                    bounded_detail::WaiterState::kCancelled,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire);
            appendDeferredWake(wakeHead, wakeTail, waiter, true);
        }
        return bounded_detail::WaiterProgress::kSkipped;
    }

    /** @brief 保留旧白盒签名；生产路径只通过唯一 pump 调用三参数版本。 */
    bounded_detail::WaiterProgress tryCompleteSendWaiter(
        const WaiterPtr& waiter, bool wake) noexcept
    {
        WaiterPtr wakeHead;
        WaiterPtr wakeTail;
        const auto progress =
            tryCompleteSendWaiter(waiter, wakeHead, wakeTail);
        if (wake) {
            issueDeferredWakes(std::move(wakeHead));
        }
        return progress;
    }

    /** @brief 保留旧白盒签名；生产路径只通过唯一 pump 调用三参数版本。 */
    bounded_detail::WaiterProgress tryCompleteRecvWaiter(
        const WaiterPtr& waiter, bool wake) noexcept
    {
        WaiterPtr wakeHead;
        WaiterPtr wakeTail;
        const auto progress =
            tryCompleteRecvWaiter(waiter, wakeHead, wakeTail);
        if (wake) {
            issueDeferredWakes(std::move(wakeHead));
        }
        return progress;
    }

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

    void drainRecvWaiters(WaiterPtr& wakeHead, WaiterPtr& wakeTail) noexcept
    {
        WaiterPtr waiter;
        while (tryDequeueWaiter(m_recvWaiters, waiter)) {
            const auto progress =
                tryCompleteRecvWaiter(waiter, wakeHead, wakeTail);
            // 重排队先增加新入口，再释放旧入口，避免计数出现零窗口。
            decrementWaiterCount(m_recvWaiterCount);
            if (progress == bounded_detail::WaiterProgress::kWaiting) {
                return;
            }
        }
    }

    void drainSendWaiters(WaiterPtr& wakeHead, WaiterPtr& wakeTail) noexcept
    {
        WaiterPtr waiter;
        while (tryDequeueWaiter(m_sendWaiters, waiter)) {
            const auto progress =
                tryCompleteSendWaiter(waiter, wakeHead, wakeTail);
            // 重排队先增加新入口，再释放旧入口，避免计数出现零窗口。
            decrementWaiterCount(m_sendWaiterCount);
            if (progress == bounded_detail::WaiterProgress::kWaiting) {
                return;
            }
        }
    }

    /** @note ownership 释放后只访问本地 waiter 链，inline resume 可安全销毁 channel。 */
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

            bounded_detail::invokeTestHook(
                bounded_detail::TestHookPoint::kPumpBeforeRelease);
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

    // 生产者写 tail，唯一逻辑消费者写 head；分离缓存行避免一致性流量互相干扰。
    alignas(kCacheLine) std::atomic<size_t> m_tail{0};
    alignas(kCacheLine) std::atomic<size_t> m_head{0};
    alignas(kCacheLine) std::atomic<uint8_t> m_pumpState{0};
    // 计数覆盖已排队及 pump 正在处理的入口，重排队期间不会短暂降为零。
    alignas(kCacheLine) std::atomic<size_t> m_recvWaiterCount{0};
    alignas(kCacheLine) std::atomic<size_t> m_sendWaiterCount{0};
    const size_t m_capacity;
    const size_t m_mask;
    std::vector<Slot> m_slots;
    WaiterQueue m_recvWaiters;
    WaiterQueue m_sendWaiters;
};

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

    auto waiter = std::make_shared<bounded_detail::ChannelWaiter<T>>(
        Waker(handle), std::move(m_timeoutTimer));
    waiter->value.emplace(std::move(m_value));
    m_waiter = waiter;
    if (!channel->enqueueWaiter(channel->m_sendWaiters,
                                channel->m_sendWaiterCount,
                                waiter)) {
        waiter->state.store(bounded_detail::WaiterState::kFailed,
                            std::memory_order_release);
        return false;
    }
    channel->synchronizeSendWaiterPath();
    if (channel->m_sendWaiterCount.load(std::memory_order_seq_cst) != 0) {
        channel->requestPump(BoundedChannel<T>::kSendWork);
    }
    bounded_detail::invokeTestHook(
        bounded_detail::TestHookPoint::kSendBeforeArm);
    if (waiter->timeoutTimer != nullptr) {
        return true;
    }
    return waiter->completionWaker->arm();
}

template <BoundedValue T>
inline std::expected<void, IOError> BoundedSendAwaitable<T>::await_resume() noexcept
{
    if (m_sent) {
        return {};
    }
    if (m_waiter) {
        bounded_detail::waitForFulfillment(*m_waiter);
        const auto state = m_waiter->state.load(std::memory_order_acquire);
        m_waiter->completionWaker->clearWaker();
        if (state == bounded_detail::WaiterState::kFulfilled) {
            return {};
        }
        if (state == bounded_detail::WaiterState::kClosed) {
            return std::unexpected(IOError(kClosed, 0));
        }
        if (state == bounded_detail::WaiterState::kFailed) {
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
    bounded_detail::WaiterState expected = bounded_detail::WaiterState::kWaiting;
    bool cancelled = false;
    if (m_waiter->state.compare_exchange_strong(
            expected,
            bounded_detail::WaiterState::kCancelled,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        cancelled = true;
        m_waiter->value.reset();
        m_waiter->completionWaker->clearWaker();
    } else if (expected == bounded_detail::WaiterState::kCancelled) {
        cancelled = true;
        m_waiter->completionWaker->clearWaker();
    }
    if (cancelled) {
        if (m_channel->m_sendWaiterCount.load(std::memory_order_seq_cst) != 0) {
            m_channel->requestPump(BoundedChannel<T>::kSendWork);
        }
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

    auto waiter = std::make_shared<bounded_detail::ChannelWaiter<T>>(
        Waker(handle), std::move(m_timeoutTimer));
    m_waiter = waiter;
    if (!channel->enqueueWaiter(channel->m_recvWaiters,
                                channel->m_recvWaiterCount,
                                waiter)) {
        waiter->state.store(bounded_detail::WaiterState::kFailed,
                            std::memory_order_release);
        return false;
    }
    channel->synchronizeRecvWaiterPath();
    if (channel->m_recvWaiterCount.load(std::memory_order_seq_cst) != 0) {
        channel->requestPump(BoundedChannel<T>::kRecvWork);
    }
    bounded_detail::invokeTestHook(
        bounded_detail::TestHookPoint::kRecvBeforeArm);
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
        bounded_detail::waitForFulfillment(*m_waiter);
        const auto state = m_waiter->state.load(std::memory_order_acquire);
        if (state == bounded_detail::WaiterState::kFulfilled &&
            m_waiter->value.has_value()) {
            T value = std::move(*m_waiter->value);
            m_waiter->value.reset();
            m_waiter->completionWaker->clearWaker();
            return value;
        }
        m_waiter->completionWaker->clearWaker();
        if (state == bounded_detail::WaiterState::kClosed) {
            return std::unexpected(IOError(kClosed, 0));
        }
        if (state == bounded_detail::WaiterState::kFailed) {
            return std::unexpected(IOError(kNotReady, 0));
        }
        if (state == bounded_detail::WaiterState::kFulfilled) {
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
    bounded_detail::WaiterState expected = bounded_detail::WaiterState::kWaiting;
    bool cancelled = false;
    if (m_waiter->state.compare_exchange_strong(
            expected,
            bounded_detail::WaiterState::kCancelled,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        cancelled = true;
        m_waiter->completionWaker->clearWaker();
    } else if (expected == bounded_detail::WaiterState::kCancelled) {
        cancelled = true;
        m_waiter->completionWaker->clearWaker();
    }
    if (cancelled) {
        if (m_channel->m_recvWaiterCount.load(std::memory_order_seq_cst) != 0) {
            m_channel->requestPump(BoundedChannel<T>::kRecvWork);
        }
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

    auto waiter = std::make_shared<bounded_detail::ChannelWaiter<T>>(
        Waker(handle), std::move(m_timeoutTimer));
    m_waiter = waiter;
    if (!channel->enqueueWaiter(channel->m_recvWaiters,
                                channel->m_recvWaiterCount,
                                waiter)) {
        waiter->state.store(bounded_detail::WaiterState::kFailed,
                            std::memory_order_release);
        return false;
    }
    channel->synchronizeRecvWaiterPath();
    if (channel->m_recvWaiterCount.load(std::memory_order_seq_cst) != 0) {
        channel->requestPump(BoundedChannel<T>::kRecvWork);
    }
    bounded_detail::invokeTestHook(
        bounded_detail::TestHookPoint::kRecvBeforeArm);
    if (waiter->timeoutTimer != nullptr) {
        return true;
    }
    return waiter->completionWaker->arm();
}

template <BoundedValue T>
inline std::expected<std::vector<T>, IOError>
BoundedRecvBatchAwaitable<T>::await_resume() noexcept
{
    if (m_ready.has_value()) {
        return std::move(*m_ready);
    }
    if (m_waiter) {
        bounded_detail::waitForFulfillment(*m_waiter);
        const auto state = m_waiter->state.load(std::memory_order_acquire);
        if (state == bounded_detail::WaiterState::kFulfilled &&
            m_waiter->value.has_value()) {
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
        if (state == bounded_detail::WaiterState::kClosed ||
            state == bounded_detail::WaiterState::kFulfilled) {
            return std::unexpected(IOError(kClosed, 0));
        }
        if (state == bounded_detail::WaiterState::kFailed) {
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
    bounded_detail::WaiterState expected = bounded_detail::WaiterState::kWaiting;
    bool cancelled = false;
    if (m_waiter->state.compare_exchange_strong(
            expected,
            bounded_detail::WaiterState::kCancelled,
            std::memory_order_acq_rel,
            std::memory_order_acquire)) {
        cancelled = true;
        m_waiter->completionWaker->clearWaker();
    } else if (expected == bounded_detail::WaiterState::kCancelled) {
        cancelled = true;
        m_waiter->completionWaker->clearWaker();
    }
    if (cancelled) {
        if (m_channel->m_recvWaiterCount.load(std::memory_order_seq_cst) != 0) {
            m_channel->requestPump(BoundedChannel<T>::kRecvWork);
        }
    }
}

} // namespace galay::mpsc

#endif // GALAY_CONCURRENCY_MPSC_BOUNDED_CHANNEL_H
