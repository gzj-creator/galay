/**
 * @file bounded_channel.h
 * @brief 有界 MPMC 异步通道
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 通道使用 Vyukov 风格的有界环形队列作为数据通路，使用无锁等待队列
 * 管理挂起的发送者和接收者。满或空时，异步操作只挂起协程，不阻塞调度器线程。
 */

#ifndef GALAY_KERNEL_BOUNDED_CHANNEL_H
#define GALAY_KERNEL_BOUNDED_CHANNEL_H

#include "../common/error.h"
#include "../core/waker.h"
#include "../core/timeout.hpp"
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
#include <utility>
#include <vector>

namespace galay::kernel
{

/**
 * @brief 约束 BoundedChannel 可存储的元素类型。
 * @tparam T 元素类型；必须可移动构造和移动赋值。
 */
template <typename T>
concept BoundedValue = std::movable<T>;

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
    kNotClaimed, ///< waiter 已被其他路径认领或参数无效。
    kWaiting,    ///< waiter 尚未完成并已重新进入等待队列。
    kCompleted   ///< waiter 已成功、关闭或失败结束。
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
    Waker waker;
    std::atomic<BoundedWaiterState> state{BoundedWaiterState::kWaiting};
    std::atomic<bool> wakeIssued{false};

    /**
     * @brief 创建绑定指定协程唤醒器的等待体。
     * @param waiter_waker 等待操作完成时使用的唤醒器，所有权移入 waiter。
     */
    explicit BoundedChannelWaiter(Waker waiter_waker)
        : waker(std::move(waiter_waker)) {}
};

template <BoundedValue T>
class BoundedChannel;

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

    BoundedChannel<T>* m_channel;
    T m_value;
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

    BoundedChannel<T>* m_channel;
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

    BoundedChannel<T>* m_channel;
    size_t m_count;
    std::optional<std::vector<T>> m_ready;
    std::shared_ptr<BoundedChannelWaiter<T>> m_waiter;
    bool m_timedOut = false;
};

/**
 * @brief 固定容量、线程安全的 MPMC 异步通道。
 * @tparam T 元素类型，要求可移动构造和移动赋值。
 *
 * @details
 * - 容量会向上取整为不小于 2 的 2 的幂，可通过 capacity() 获取实际容量。
 * - trySend() / tryRecv() 不等待 Full/Empty 条件；同侧 cursor 竞争会在当前调用内
 *   通过 CPU hint 退避重试，不主动进入调度器。
 * - send() / recv() / recvBatch() 满或空时只挂起协程，不阻塞底层线程。
 * - 多生产者之间不保证全局顺序；每个生产者自身发送的消息保持 FIFO。
 *
 * @note 通道是身份对象，不可复制或移动。通道必须存活到所有挂起操作完成或超时。
 * @note trySend(T&&) 只有在成功抢到 slot 或完成直接交接后才移动参数；失败时参数保持未移动。
 *       需要失败后拿回值重试时应使用 trySend()。co_await send() 的值会先移动到等待体，
 *       超时或关闭时不会归还。
 */
template <BoundedValue T>
class BoundedChannel
{
public:
    static_assert(BoundedValue<T>, "BoundedChannel requires a movable T");

    /**
     * @brief 构造有界通道。
     * @param capacity 期望容量；小于等于 2 时实际容量为 2，其余向上取整到 2 的幂。
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
        const size_t head = m_head.load(std::memory_order_relaxed);
        const size_t tail = m_tail.load(std::memory_order_relaxed);
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
     * @note CAS 竞争在当前调用内通过 CPU hint 重试，不因输掉 cursor 认领而返回 false。
     */
    bool trySend(T&& value)
    {
        if (isClosed()) {
            return false;
        }
        const bool recvWaiterPathUsed =
            m_recvWaiterPathUsed.load(std::memory_order_relaxed);
        if (recvWaiterPathUsed &&
            tryHandoffToConsumer(value)) {
            return true;
        }
        if (recvWaiterPathUsed && isClosed()) {
            return false;
        }
        if (!ringEnqueue(std::move(value))) {
            return false;
        }
        if (m_recvWaiterPathUsed.load(std::memory_order_relaxed)) {
            wakeOneConsumerIfAny();
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
     * @note CAS 竞争在当前调用内通过 CPU hint 重试，不因输掉 cursor 认领而返回空结果。
     */
    std::optional<T> tryRecv()
    {
        std::optional<T> value;
        if (ringDequeueTo([&value](T&& item) {
                value.emplace(std::move(item));
            })) {
            if (m_sendWaiterPathUsed.load(std::memory_order_relaxed)) {
                drainOneSendWaiter();
            }
            return value;
        }
        if (m_sendWaiterPathUsed.load(std::memory_order_relaxed) &&
            tryTakeFromSender([&value](T&& item) {
                value.emplace(std::move(item));
            })) {
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
     * @details 关闭状态通过原子变量发布，可与发送、接收及其他 close() 调用并发执行。
     * @note 操作幂等；关闭后发送失败，接收仍会先排空 ring 中的残留消息。
     */
    void close() noexcept
    {
        if (m_closed.exchange(true, std::memory_order_acq_rel)) {
            return;
        }
        wakeAllWaiters(m_recvWaiters);
        wakeAllWaiters(m_sendWaiters);
    }

    /**
     * @brief 查询通道是否已关闭。
     * @return true 表示已关闭。
     * @note 返回调用时刻的原子快照，可与其他通道操作并发调用。
     */
    bool isClosed() const noexcept
    {
        return m_closed.load(std::memory_order_acquire);
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
     * @return 仅供诊断，不可用作同步条件；直接交接的消息不计入此值。
     */
    size_t size() const noexcept
    {
        const size_t tail = m_tail.load(std::memory_order_relaxed);
        const size_t head = m_head.load(std::memory_order_relaxed);
        if (tail < head) {
            return 0;
        }
        const size_t count = tail - head;
        return count < m_capacity ? count : m_capacity;
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
    // Apple silicon 的一致性粒度是 128B，x86 是 64B。取 64 会让 m_head/m_tail
    // 落在同一条 128B 行上，生产者与消费者的热索引持续互相无效化。
#if defined(__aarch64__) || defined(__ARM_ARCH_ISA_A64)
    static constexpr size_t kCacheLine = 128;
#else
    static constexpr size_t kCacheLine = 64;
#endif

    struct Slot
    {
        alignas(T) std::byte storage[sizeof(T)];
        std::atomic<size_t> sequence{0};
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

    /**
     * @brief 唤醒 waiter 关联的协程并清空其唤醒器。
     * @param waiter 已完成状态发布的等待体。
     * @note 调用方必须保证同一 waiter 至多执行一次实际唤醒。
     */
    void wakeWaiter(Waiter& waiter) noexcept
    {
        waiter.waker.wakeUp();
        waiter.waker = Waker();
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
        return std::bit_ceil(capacity);
    }

    /**
     * @brief 尝试在 ring 中认领一个生产者 slot 并发布消息。
     * @param value 待发布消息；仅在成功认领 slot 后移动。
     * @return 发布成功返回 true；当前 ring 无可用 slot 时返回 false。
     * @note 支持多个生产者并发调用；CAS 竞争使用 CPU hint 重试，slot 发布等待可能让出线程时间片。
     */
    bool ringEnqueue(T&& value) noexcept
    {
        size_t position = m_tail.load(std::memory_order_relaxed);
        Slot* slot = nullptr;
        detail::BoundedChannelBackoff backoff;
        for (;;) {
            slot = &m_slots[position & m_mask];
            const size_t sequence = slot->sequence.load(std::memory_order_acquire);
            const auto difference = static_cast<std::ptrdiff_t>(sequence) -
                static_cast<std::ptrdiff_t>(position);
            if (difference == 0) {
                if (m_tail.compare_exchange_weak(position,
                                                 position + 1,
                                                 std::memory_order_relaxed,
                                                 std::memory_order_relaxed)) {
                    break;
                }
                backoff.spin();
            } else if (difference < 0) {
                return false;
            } else {
                position = m_tail.load(std::memory_order_relaxed);
                backoff.snooze();
            }
        }

        std::construct_at(slot->value(), std::move(value));
        slot->sequence.store(position + 1, std::memory_order_release);
        return true;
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
        size_t position = m_head.load(std::memory_order_relaxed);
        Slot* slot = nullptr;
        detail::BoundedChannelBackoff backoff;
        for (;;) {
            slot = &m_slots[position & m_mask];
            const size_t sequence = slot->sequence.load(std::memory_order_acquire);
            const auto difference = static_cast<std::ptrdiff_t>(sequence) -
                static_cast<std::ptrdiff_t>(position + 1);
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
        slot->sequence.store(position + m_capacity, std::memory_order_release);
        return true;
    }

    /**
     * @brief 尝试把待发送值直接交给一个正在等待的接收者。
     * @param value 待交接值；仅在成功认领接收 waiter 后移动。
     * @return 成功完成直接交接并唤醒接收者时返回 true；没有可认领 waiter 时返回 false。
     * @note 直接交接不经过 ring，因此不会改变 size() 返回的近似计数。
     */
    bool tryHandoffToConsumer(T& value) noexcept
    {
        WaiterPtr waiter;
        while (m_recvWaiters.try_dequeue(waiter)) {
            if (!waiter) {
                continue;
            }
            BoundedWaiterState expected = BoundedWaiterState::kWaiting;
            if (!waiter->state.compare_exchange_strong(expected,
                                                       BoundedWaiterState::kFulfilling,
                                                       std::memory_order_acq_rel,
                                                       std::memory_order_acquire)) {
                continue;
            }
            waiter->value.emplace(std::move(value));
            waiter->wakeIssued.store(true, std::memory_order_release);
            waiter->state.store(BoundedWaiterState::kFulfilled, std::memory_order_release);
            wakeWaiter(*waiter);
            return true;
        }
        return false;
    }

    /**
     * @brief 尝试从等待队列认领一个挂起的发送者并直接消费其值。
     * @tparam Consume 接收 T&& 的不可抛出消费回调类型。
     * @param consume 认领成功后处理发送值的回调。
     * @return 成功交接并唤醒发送者时返回 true；通道关闭或没有可认领 waiter 时返回 false。
     */
    template <typename Consume>
    bool tryTakeFromSender(Consume&& consume) noexcept
    {
        if (isClosed()) {
            return false;
        }

        WaiterPtr waiter;
        while (m_sendWaiters.try_dequeue(waiter)) {
            if (!waiter) {
                continue;
            }
            BoundedWaiterState expected = BoundedWaiterState::kWaiting;
            if (!waiter->state.compare_exchange_strong(expected,
                                                       BoundedWaiterState::kFulfilling,
                                                       std::memory_order_acq_rel,
                                                       std::memory_order_acquire)) {
                continue;
            }
            if (!waiter->value.has_value()) {
                waiter->wakeIssued.store(true, std::memory_order_release);
                waiter->state.store(BoundedWaiterState::kClosed, std::memory_order_release);
                wakeWaiter(*waiter);
                continue;
            }
            consume(std::move(*waiter->value));
            waiter->value.reset();
            waiter->wakeIssued.store(true, std::memory_order_release);
            waiter->state.store(BoundedWaiterState::kFulfilled, std::memory_order_release);
            wakeWaiter(*waiter);
            return true;
        }
        return false;
    }

    /**
     * @brief 尝试推进一个发送 waiter 的状态。
     * @param waiter 待推进的共享等待体；空指针视为未认领。
     * @param wake 完成、关闭或失败时是否立即唤醒关联协程。
     * @return kCompleted 表示等待体已结束，kWaiting 表示已重新排队，
     *         kNotClaimed 表示等待体已由其他并发路径处理。
     * @note ring 仍满时会把 waiter 恢复为 kWaiting 并重新入队。
     */
    BoundedWaiterProgress tryCompleteSendWaiter(const WaiterPtr& waiter, bool wake) noexcept
    {
        if (!waiter) {
            return BoundedWaiterProgress::kNotClaimed;
        }

        if (isClosed()) {
            BoundedWaiterState expected = BoundedWaiterState::kWaiting;
            if (!waiter->state.compare_exchange_strong(expected,
                                                       BoundedWaiterState::kFulfilling,
                                                       std::memory_order_acq_rel,
                                                       std::memory_order_acquire)) {
                return BoundedWaiterProgress::kNotClaimed;
            }
            if (wake) {
                waiter->wakeIssued.store(true, std::memory_order_release);
            }
            waiter->state.store(BoundedWaiterState::kClosed, std::memory_order_release);
            if (wake) {
                wakeWaiter(*waiter);
            }
            return BoundedWaiterProgress::kCompleted;
        }

        BoundedWaiterState expected = BoundedWaiterState::kWaiting;
        if (!waiter->state.compare_exchange_strong(expected,
                                                   BoundedWaiterState::kFulfilling,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
            return BoundedWaiterProgress::kNotClaimed;
        }

        if (waiter->value.has_value() && ringEnqueue(std::move(*waiter->value))) {
            waiter->value.reset();
            if (wake) {
                waiter->wakeIssued.store(true, std::memory_order_release);
            }
            waiter->state.store(BoundedWaiterState::kFulfilled, std::memory_order_release);
            if (wake) {
                wakeWaiter(*waiter);
            }
            return BoundedWaiterProgress::kCompleted;
        }

        waiter->state.store(BoundedWaiterState::kWaiting, std::memory_order_release);
        if (!m_sendWaiters.enqueue(waiter)) {
            if (wake) {
                waiter->wakeIssued.store(true, std::memory_order_release);
            }
            waiter->state.store(BoundedWaiterState::kFailed, std::memory_order_release);
            if (wake) {
                wakeWaiter(*waiter);
            }
            return BoundedWaiterProgress::kCompleted;
        }
        if (isClosed()) {
            wakeAllWaiters(m_sendWaiters);
        }
        return BoundedWaiterProgress::kWaiting;
    }

    /**
     * @brief 尝试推进一个接收 waiter 的状态。
     * @param waiter 待推进的共享等待体；空指针视为未认领。
     * @param wake 完成、关闭或失败时是否立即唤醒关联协程。
     * @return kCompleted 表示等待体已结束，kWaiting 表示已重新排队，
     *         kNotClaimed 表示等待体已由其他并发路径处理。
     * @note ring 仍空时会把 waiter 恢复为 kWaiting 并重新入队。
     */
    BoundedWaiterProgress tryCompleteRecvWaiter(const WaiterPtr& waiter, bool wake) noexcept
    {
        if (!waiter) {
            return BoundedWaiterProgress::kNotClaimed;
        }

        BoundedWaiterState expected = BoundedWaiterState::kWaiting;
        if (!waiter->state.compare_exchange_strong(expected,
                                                   BoundedWaiterState::kFulfilling,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire)) {
            return BoundedWaiterProgress::kNotClaimed;
        }

        if (ringDequeueTo([&waiter](T&& value) {
                waiter->value.emplace(std::move(value));
            })) {
            if (wake) {
                waiter->wakeIssued.store(true, std::memory_order_release);
            }
            waiter->state.store(BoundedWaiterState::kFulfilled, std::memory_order_release);
            drainOneSendWaiter();
            if (wake) {
                wakeWaiter(*waiter);
            }
            return BoundedWaiterProgress::kCompleted;
        }

        if (isClosed()) {
            if (wake) {
                waiter->wakeIssued.store(true, std::memory_order_release);
            }
            waiter->state.store(BoundedWaiterState::kClosed, std::memory_order_release);
            if (wake) {
                wakeWaiter(*waiter);
            }
            return BoundedWaiterProgress::kCompleted;
        }

        waiter->state.store(BoundedWaiterState::kWaiting, std::memory_order_release);
        if (!m_recvWaiters.enqueue(waiter)) {
            if (wake) {
                waiter->wakeIssued.store(true, std::memory_order_release);
            }
            waiter->state.store(BoundedWaiterState::kFailed, std::memory_order_release);
            if (wake) {
                wakeWaiter(*waiter);
            }
            return BoundedWaiterProgress::kCompleted;
        }
        if (isClosed()) {
            wakeAllWaiters(m_recvWaiters);
        }
        return BoundedWaiterProgress::kWaiting;
    }

    /**
     * @brief 尝试推进并唤醒一个等待中的接收者。
     * @note 跳过已取消或被其他路径认领的 waiter；处理一个有效 waiter 后立即返回。
     */
    void wakeOneConsumerIfAny() noexcept
    {
        WaiterPtr waiter;
        while (m_recvWaiters.try_dequeue(waiter)) {
            const auto progress = tryCompleteRecvWaiter(waiter, true);
            if (progress == BoundedWaiterProgress::kCompleted ||
                progress == BoundedWaiterProgress::kWaiting) {
                return;
            }
        }
    }

    /**
     * @brief 在消费者释放容量后尝试推进一个等待中的发送者。
     * @note 跳过已取消或被其他路径认领的 waiter；处理一个有效 waiter 后立即返回。
     */
    void drainOneSendWaiter() noexcept
    {
        WaiterPtr waiter;
        while (m_sendWaiters.try_dequeue(waiter)) {
            const auto progress = tryCompleteSendWaiter(waiter, true);
            if (progress == BoundedWaiterProgress::kCompleted ||
                progress == BoundedWaiterProgress::kWaiting) {
                return;
            }
        }
    }

    /**
     * @brief 将指定等待队列中尚处于 kWaiting 的 waiter 标记为关闭并全部唤醒。
     * @param waiters 待清理的发送或接收等待队列。
     * @note 已取消、已完成或正在搬运值的 waiter 由其当前认领方负责收尾。
     */
    void wakeAllWaiters(WaiterQueue& waiters) noexcept
    {
        WaiterPtr waiter;
        while (waiters.try_dequeue(waiter)) {
            if (!waiter) {
                continue;
            }
            BoundedWaiterState expected = BoundedWaiterState::kWaiting;
            if (waiter->state.compare_exchange_strong(expected,
                                                       BoundedWaiterState::kFulfilling,
                                                       std::memory_order_acq_rel,
                                                       std::memory_order_acquire)) {
                waiter->wakeIssued.store(true, std::memory_order_release);
                waiter->state.store(BoundedWaiterState::kClosed, std::memory_order_release);
                wakeWaiter(*waiter);
            }
        }
    }

    template <BoundedValue U>
    friend class BoundedSendAwaitable;
    template <BoundedValue U>
    friend class BoundedRecvAwaitable;
    template <BoundedValue U>
    friend class BoundedRecvBatchAwaitable;

    alignas(kCacheLine) std::atomic<size_t> m_tail{0};
    alignas(kCacheLine) std::atomic<size_t> m_head{0};
    alignas(kCacheLine) std::atomic<bool> m_closed{false};
    // Once an async waiter path has been used, keep the waiter-aware path enabled.
    // Before that point, synchronous trySend/tryRecv avoid polling empty waiter queues.
    alignas(kCacheLine) std::atomic<bool> m_recvWaiterPathUsed{false};
    alignas(kCacheLine) std::atomic<bool> m_sendWaiterPathUsed{false};
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

    auto waiter = std::make_shared<BoundedChannelWaiter<T>>(Waker(handle));
    waiter->value.emplace(std::move(m_value));
    m_waiter = waiter;
    channel->m_sendWaiterPathUsed.store(true, std::memory_order_relaxed);
    if (!channel->m_sendWaiters.enqueue(waiter)) {
        waiter->state.store(BoundedWaiterState::kFailed, std::memory_order_release);
        return false;
    }

    const auto progress = channel->tryCompleteSendWaiter(waiter, false);
    if (progress == BoundedWaiterProgress::kCompleted) {
        return false;
    }
    return true;
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
        if (!m_waiter->wakeIssued.load(std::memory_order_acquire)) {
            m_waiter->waker = Waker();
        }
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
    if (!m_waiter) {
        m_timedOut = true;
        return;
    }
    BoundedWaiterState expected = BoundedWaiterState::kWaiting;
    if (m_waiter->state.compare_exchange_strong(expected,
                                                BoundedWaiterState::kCancelled,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
        m_timedOut = true;
        if (!m_waiter->wakeIssued.load(std::memory_order_acquire)) {
            m_waiter->waker = Waker();
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
    return m_channel->isClosed();
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

    auto waiter = std::make_shared<BoundedChannelWaiter<T>>(Waker(handle));
    m_waiter = waiter;
    channel->m_recvWaiterPathUsed.store(true, std::memory_order_relaxed);
    if (!channel->m_recvWaiters.enqueue(waiter)) {
        waiter->state.store(BoundedWaiterState::kFailed, std::memory_order_release);
        return false;
    }

    const auto progress = channel->tryCompleteRecvWaiter(waiter, false);
    if (progress == BoundedWaiterProgress::kCompleted) {
        return false;
    }
    return true;
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
            if (!m_waiter->wakeIssued.load(std::memory_order_acquire)) {
                m_waiter->waker = Waker();
            }
            return value;
        }
        if (!m_waiter->wakeIssued.load(std::memory_order_acquire)) {
            m_waiter->waker = Waker();
        }
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
    if (m_channel->isClosed()) {
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
    if (!m_waiter) {
        m_timedOut = true;
        return;
    }
    BoundedWaiterState expected = BoundedWaiterState::kWaiting;
    if (m_waiter->state.compare_exchange_strong(expected,
                                                BoundedWaiterState::kCancelled,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
        m_timedOut = true;
        if (!m_waiter->wakeIssued.load(std::memory_order_acquire)) {
            m_waiter->waker = Waker();
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
    return m_channel->isClosed();
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

    auto waiter = std::make_shared<BoundedChannelWaiter<T>>(Waker(handle));
    m_waiter = waiter;
    channel->m_recvWaiterPathUsed.store(true, std::memory_order_relaxed);
    if (!channel->m_recvWaiters.enqueue(waiter)) {
        waiter->state.store(BoundedWaiterState::kFailed, std::memory_order_release);
        return false;
    }

    const auto progress = channel->tryCompleteRecvWaiter(waiter, false);
    if (progress == BoundedWaiterProgress::kCompleted) {
        return false;
    }
    return true;
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
            if (!m_waiter->wakeIssued.load(std::memory_order_acquire)) {
                m_waiter->waker = Waker();
            }
            return values;
        }
        if (!m_waiter->wakeIssued.load(std::memory_order_acquire)) {
            m_waiter->waker = Waker();
        }
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
    if (m_channel->isClosed()) {
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
    if (!m_waiter) {
        m_timedOut = true;
        return;
    }
    BoundedWaiterState expected = BoundedWaiterState::kWaiting;
    if (m_waiter->state.compare_exchange_strong(expected,
                                                BoundedWaiterState::kCancelled,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
        m_timedOut = true;
        m_waiter->waker = Waker();
    }
}


} // namespace galay::kernel

#endif // GALAY_KERNEL_BOUNDED_CHANNEL_H
