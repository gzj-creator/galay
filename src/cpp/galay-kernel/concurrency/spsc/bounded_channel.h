/**
 * @file bounded_channel.h
 * @brief 有界 SPSC 异步通道
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 异步控制面需要 close/waiter 竞态裁决，因此每个 slot 使用三态 ready
 * 字段完成发布握手，但不需要 MPSC/MPMC 的 sequence generation。等待路径使用
 * 单 waiter slot 管理挂起协程，满或空时只挂起协程，不阻塞调度器线程。纯轮询且
 * 不需要 close/timeout 的工作负载应使用本头导出的 Ring，其热路径没有逐 slot 原子。
 */

#ifndef GALAY_CONCURRENCY_SPSC_BOUNDED_CHANNEL_H
#define GALAY_CONCURRENCY_SPSC_BOUNDED_CHANNEL_H

#include "../../common/error.h"
#include "../../core/waker.h"
#include "../../core/timeout.hpp"
#include "../detail/asymmetric_memory_barrier.h"
#include "../../../galay-utils/common/defn.hpp"
#include "../../../galay-utils/cache/type_ring_buffer.hpp"
#include <coroutine>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <emmintrin.h>
#endif
#include <array>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

namespace galay::spsc
{

using kernel::IOError;
using kernel::TimeoutTimer;
using kernel::TimeoutSupport;
using kernel::Waker;
using kernel::WithTimeout;
using kernel::kClosed;
using kernel::kNotReady;
using kernel::kTimeout;

/** @brief 约束 Ring 可保存且能在无异常热路径中移动的元素类型。 */
template <typename T>
concept RingValue = ::galay::utils::TypeRingBufferValue<T>;

/** @brief 约束 Ring 使用无锁无符号整数游标。 */
template <typename Cursor>
concept RingCursor = ::galay::utils::TypeRingBufferCursor<Cursor>;

/** @brief Ring 构造状态。 */
using RingError = ::galay::utils::TypeRingBufferError;

/**
 * @brief 获取 RingError 的静态错误字符串。
 *
 * @param error 错误枚举。
 * @return 覆盖所有公开枚举值的非空字符串。
 */
[[nodiscard]] inline const char* ringErrorString(RingError error) noexcept
{
    return ::galay::utils::typeRingBufferErrorString(error);
}

/**
 * @brief 无 waiter 的固定容量 1P1C ring。
 * @tparam T 不抛移动构造和析构的元素类型；输出缓冲接口另行约束移动赋值。
 * @tparam Cursor 无锁无符号游标类型，默认 size_t。
 * @note 只能有一个逻辑生产者和一个逻辑消费者；析构前必须停止两侧访问。
 */
template <RingValue T, RingCursor Cursor = size_t>
using Ring = ::galay::utils::DynamicTypeRingBuffer<T, Cursor>;

/**
 * @brief 编译期容量、成员内持有槽位的无 waiter 1P1C ring。
 * @tparam T 不抛移动构造和析构的元素类型。
 * @tparam Capacity 不小于 2 且处于 Cursor 安全半区间的 2 次幂容量。
 * @tparam Cursor 无锁无符号游标类型，默认 size_t。
 * @note 只能默认构造；构造和稳定数据面均不分配内存。
 * @note 槽位直接属于对象；大 Capacity 或大 T 不应放入小线程栈或 coroutine frame。
 */
template <RingValue T, size_t Capacity, RingCursor Cursor = size_t>
using StaticRing = ::galay::utils::StaticTypeRingBuffer<T, Capacity, Cursor>;

/**
 * @brief 约束 BoundedChannel 可存储的元素类型。
 * @tparam T 元素类型；必须可移动，且移动构造和析构不得抛出异常。
 */
template <typename T>
concept BoundedValue = std::movable<T> &&
    std::is_nothrow_move_constructible_v<T> &&
    std::is_nothrow_destructible_v<T>;

namespace detail {

/**
 * @brief 执行一次平台相关的短时 CPU 自旋提示。
 *
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

/** @brief 约束异步 bounded channel 的动态或安全编译期容量。 */
template <size_t Capacity>
concept ValidBoundedChannelCapacity =
    Capacity == std::dynamic_extent ||
    (Capacity >= 2 && std::has_single_bit(Capacity) &&
     Capacity <= (size_t{1} <<
         (std::numeric_limits<size_t>::digits - 1U)));


} // namespace detail

enum class BoundedWaiterState : uint8_t {
    kIdle,          ///< 固定 waiter 槽当前未被任何异步操作占用。
    kRegistering,   ///< await_suspend 正在初始化尚未发布的 waiter 槽。
    kWaiting,       ///< 已注册但尚未被认领
    kCancelled,     ///< 已由超时路径取消
    kFulfilling,    ///< 对端已认领，正在搬运值
    kFulfilled,     ///< 已成功交接
    kClosed,        ///< 因通道关闭结束
    kFailed,        ///< 等待队列无法接收 waiter，返回 kNotReady
    kReclaiming     ///< await_resume 正在等待旧推进方释放槽引用。
};

enum class BoundedWaiterProgress : uint8_t {
    kNotClaimed, ///< waiter 已被其他路径认领或参数无效。
    kWaiting,    ///< waiter 尚未完成并已重新进入等待队列。
    kCompleted,  ///< waiter 已成功、关闭或失败结束。
    kSkipped     ///< waiter 已超时或此前完成，调用方应继续扫描。
};

enum class BoundedWaiterWakePhase : uint8_t {
    kArming,      ///< await_suspend 尚未完成最后一次共享状态发布。
    kArmed,       ///< await_suspend 已允许对端调度恢复。
    kPendingWake, ///< arming 窗口内已完成，由当前栈直接进入 await_resume。
    kWakeIssued   ///< 已向 scheduler 提交唯一一次恢复请求。
};

/**
 * @brief BoundedChannel 的异步等待体。
 * @tparam T 通道元素类型。
 * @details 每个 channel 为发送侧和接收侧各内嵌一个固定槽。消息本身仍保存在
 *          coroutine awaiter 中，避免让大 T 使 channel 对象额外膨胀。
 */
template <BoundedValue T>
struct BoundedChannelWaiter
{
    Waker waker;
    TimeoutTimer::ptr timeoutTimer; ///< 当前 generation 的事务式超时裁决器。
    T* sendValue = nullptr; ///< 指向发送 awaiter 中等待搬运的值。
    std::optional<T>* recvValue = nullptr; ///< 单条接收的结果槽；nullptr 表示批量 waiter 只请求通知。
    std::atomic<uint64_t> generation{0};
    std::atomic<uint64_t> retryRequested{0};
    std::atomic<size_t> pins{0}; ///< 队列入口或推进方持有的固定槽引用数。
    std::atomic<BoundedWaiterState> state{BoundedWaiterState::kIdle};
    std::atomic<BoundedWaiterWakePhase> wakePhase{BoundedWaiterWakePhase::kArming};
    std::atomic<bool> wakeIssued{false};
    std::atomic<bool> queued{false};

    /**
     * @brief 为一个新 generation 初始化尚未发布的固定 waiter 槽。
     *
     * @param waiter_waker 等待操作完成时使用的唤醒器，所有权移入 waiter。
     * @param timeout_timer 可选事务式超时裁决器，等待体生命周期内保持不变。
     * @param send_value 发送操作的 awaiter-owned 值；接收操作传 nullptr。
     * @param recv_value 单条接收的 awaiter-owned 结果槽；发送或批量通知 waiter 传 nullptr。
     * @param waiter_generation 返回本次成功注册的 generation。
     * @return 槽从 kIdle 进入 kWaiting 时返回 true；同侧已有挂起操作时返回 false。
     */
    [[nodiscard]] bool begin(Waker waiter_waker,
                             TimeoutTimer::ptr timeout_timer,
                             T* send_value,
                             std::optional<T>* recv_value,
                             uint64_t& waiter_generation) noexcept
    {
        BoundedWaiterState expected = BoundedWaiterState::kIdle;
        if (!state.compare_exchange_strong(expected,
                                           BoundedWaiterState::kRegistering,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
            return false;
        }

        const uint64_t nextGeneration =
            generation.fetch_add(1, std::memory_order_relaxed) + 1;
        waker = std::move(waiter_waker);
        timeoutTimer = std::move(timeout_timer);
        sendValue = send_value;
        recvValue = recv_value;
        retryRequested.store(0, std::memory_order_relaxed);
        wakePhase.store(BoundedWaiterWakePhase::kArming,
                        std::memory_order_relaxed);
        wakeIssued.store(false, std::memory_order_relaxed);
        queued.store(false, std::memory_order_relaxed);
        waiter_generation = nextGeneration;
        state.store(BoundedWaiterState::kWaiting, std::memory_order_release);
        return true;
    }

    /**
     * @brief 在 await_suspend 的最后一步发布 waiter 可被异步唤醒。
     *
     * @return 成功发布 kArmed 时返回 true；arming 期间已有完成请求时返回 false，
     *         调用方必须由当前栈直接进入 await_resume。
     *
     * @note 成功返回后对端可立即恢复并销毁协程帧，因此调用方不得再访问 awaiter、
     *       channel 或 waiter。
     */
    [[nodiscard]] bool finishArming() noexcept
    {
        BoundedWaiterWakePhase expected = BoundedWaiterWakePhase::kArming;
        return wakePhase.compare_exchange_strong(
            expected,
            BoundedWaiterWakePhase::kArmed,
            std::memory_order_acq_rel,
            std::memory_order_acquire);
    }
};

template <BoundedValue T, size_t Capacity = std::dynamic_extent>
    requires detail::ValidBoundedChannelCapacity<Capacity>
class BoundedChannel;

template <BoundedValue T, size_t Capacity = std::dynamic_extent>
class BoundedSendAwaitable;

template <BoundedValue T, size_t Capacity = std::dynamic_extent>
class BoundedRecvAwaitable;

template <BoundedValue T, size_t Capacity = std::dynamic_extent>
class BoundedRecvBatchAwaitable;

template <BoundedValue T, size_t Capacity = std::dynamic_extent>
class BoundedRecvBatchToAwaitable;

/**
 * @brief 异步发送等待体。
 * @tparam T 通道元素类型。
 * @details 满时挂起协程而不阻塞线程；结果通过 std::expected<void, IOError> 返回。
 */
template <BoundedValue T, size_t Capacity>
class BoundedSendAwaitable
    : public TimeoutSupport<BoundedSendAwaitable<T, Capacity>>
{
public:
    /**
     * @brief 创建异步发送等待体。
     *
     * @param channel 目标通道的非拥有指针，等待体完成前必须保持有效。
     * @param value 待发送值，所有权移入等待体。
     */
    explicit BoundedSendAwaitable(BoundedChannel<T, Capacity>* channel, T&& value)
        : m_channel(channel), m_value(std::move(value)) {}

    /**
     * @brief 尝试在不挂起协程的情况下完成发送。
     *
     * @return 已发送或通道已关闭时返回 true；需要注册发送等待者时返回 false。
     */
    bool await_ready() noexcept;

    /**
     * @brief 注册发送等待者，并在注册后重新检查发送或关闭状态。
     *
     * @tparam Promise 调用方协程 promise 类型。
     * @param handle 当前调用方的协程句柄。
     * @return 需要等待接收方释放容量时返回 true；已同步完成或失败时返回 false。
     *
     * @note 该函数只挂起协程，不阻塞调度器线程。
     */
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) noexcept;

    /**
     * @brief 获取异步发送结果。
     *
     * @return 成功返回空 expected；关闭、超时、构造 OOM/容量非法或等待队列失败
     *         分别返回 IOError(kClosed)、IOError(kTimeout)、IOError(kOutOfMemory)、
     *         IOError(kParamInvalid) 或 IOError(kNotReady)。
     */
    std::expected<void, IOError> await_resume() noexcept;

    /**
     * @brief 由超时设施尝试取消尚未被对端认领的发送等待者。
     *
     * @note 已进入值搬运阶段的 waiter 不会被超时路径抢占。
     */
    void markTimeout() noexcept;

private:
    friend struct WithTimeout<BoundedSendAwaitable<T, Capacity>>;

    void bindTimeoutTimer(const TimeoutTimer::ptr& timer) noexcept
    {
        m_timeoutTimer = timer;
    }

    BoundedChannel<T, Capacity>* m_channel;
    T m_value;
    TimeoutTimer::ptr m_timeoutTimer;
    BoundedChannelWaiter<T>* m_waiter = nullptr;
    uint64_t m_waiterGeneration = 0;
    uint32_t m_registrationSystemError = 0;
    bool m_sent = false;
    bool m_timedOut = false;
    bool m_registrationFailed = false;
};

/**
 * @brief 异步单条接收等待体。
 * @tparam T 通道元素类型。
 * @details 空时挂起协程而不阻塞线程；关闭和超时通过 IOError 返回。
 */
template <BoundedValue T, size_t Capacity>
class BoundedRecvAwaitable
    : public TimeoutSupport<BoundedRecvAwaitable<T, Capacity>>
{
public:
    /**
     * @brief 创建异步单条接收等待体。
     *
     * @param channel 目标通道的非拥有指针，等待体完成前必须保持有效。
     */
    explicit BoundedRecvAwaitable(BoundedChannel<T, Capacity>* channel)
        : m_channel(channel) {}

    /**
     * @brief 尝试在不挂起协程的情况下接收一条消息。
     *
     * @return 已取得消息或通道已关闭时返回 true；需要注册接收等待者时返回 false。
     */
    bool await_ready() noexcept;

    /**
     * @brief 注册接收等待者，并在注册后重新检查数据或关闭状态。
     *
     * @tparam Promise 调用方协程 promise 类型。
     * @param handle 当前调用方的协程句柄。
     * @return 需要等待生产者提供消息时返回 true；已同步完成或失败时
     *         返回 false。
     *
     * @note 该函数只挂起协程，不阻塞调度器线程。
     */
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) noexcept;

    /**
     * @brief 获取异步单条接收结果。
     *
     * @return 成功返回收到的消息；关闭、超时、构造 OOM/容量非法或等待队列失败
     *         返回对应 IOError。
     */
    std::expected<T, IOError> await_resume() noexcept;

    /**
     * @brief 由超时设施尝试取消尚未被对端认领的接收等待者。
     *
     * @note 已进入值搬运阶段的 waiter 不会被超时路径抢占。
     */
    void markTimeout() noexcept;

private:
    friend struct WithTimeout<BoundedRecvAwaitable<T, Capacity>>;

    void bindTimeoutTimer(const TimeoutTimer::ptr& timer) noexcept
    {
        m_timeoutTimer = timer;
    }

    BoundedChannel<T, Capacity>* m_channel;
    std::optional<T> m_ready;
    TimeoutTimer::ptr m_timeoutTimer;
    BoundedChannelWaiter<T>* m_waiter = nullptr;
    uint64_t m_waiterGeneration = 0;
    uint32_t m_registrationSystemError = 0;
    bool m_timedOut = false;
    bool m_registrationFailed = false;
};

/**
 * @brief 异步批量接收等待体。
 * @tparam T 通道元素类型。
 * @details 等待至少一条消息，恢复时尽量补齐请求数量；不会阻塞调度器线程。
 * @note 返回 vector 的 convenience 路径可能分配，默认 allocator OOM 不经 IOError；
 *       需要显式可恢复、无分配接收时使用 BoundedChannel::recvBatchTo(std::span<T>)。
 */
template <BoundedValue T, size_t Capacity>
class BoundedRecvBatchAwaitable
    : public TimeoutSupport<BoundedRecvBatchAwaitable<T, Capacity>>
{
public:
    /**
     * @brief 创建异步批量接收等待体。
     *
     * @param channel 目标通道的非拥有指针，等待体完成前必须保持有效。
     * @param count 单次最多接收的消息数；0 表示立即返回空批次。
     */
    BoundedRecvBatchAwaitable(BoundedChannel<T, Capacity>* channel, size_t count)
        : m_channel(channel), m_count(count)
    {}

    /**
     * @brief 尝试在不挂起协程的情况下取得一批消息。
     *
     * @return 已取得批次或通道已关闭时返回 true；需要注册接收等待者时返回 false。
     */
    bool await_ready();

    /**
     * @brief 注册批量接收等待者，并在注册后重新检查数据或关闭状态。
     *
     * @tparam Promise 调用方协程 promise 类型。
     * @param handle 当前调用方的协程句柄。
     * @return 需要等待至少一条消息时返回 true；已同步完成或失败时返回 false。
     *
     * @note 该函数只挂起协程，不阻塞调度器线程。
     */
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle);

    /**
     * @brief 获取异步批量接收结果。
     *
     * @return 成功返回最多 count 条消息；关闭、超时、构造 OOM/容量非法或等待队列
     *         失败返回对应 IOError。
     */
    std::expected<std::vector<T>, IOError> await_resume();

    /**
     * @brief 由超时设施尝试取消尚未被对端认领的批量接收等待者。
     *
     * @note 已进入值搬运阶段的 waiter 不会被超时路径抢占。
     */
    void markTimeout() noexcept;

private:
    friend struct WithTimeout<BoundedRecvBatchAwaitable<T, Capacity>>;

    bool tryReceiveNow();
    void bindTimeoutTimer(const TimeoutTimer::ptr& timer) noexcept
    {
        m_timeoutTimer = timer;
    }

    BoundedChannel<T, Capacity>* m_channel;
    size_t m_count;
    std::vector<T> m_values;
    TimeoutTimer::ptr m_timeoutTimer;
    BoundedChannelWaiter<T>* m_waiter = nullptr;
    uint64_t m_waiterGeneration = 0;
    uint32_t m_registrationSystemError = 0;
    bool m_timedOut = false;
    bool m_registrationFailed = false;
    bool m_batchReady = false;
};

/**
 * @brief 将批量消息移动到调用方已构造缓冲区的异步等待体。
 * @tparam T 可不抛移动赋值的通道元素类型。
 * @details 空通道时挂起协程而不阻塞线程；数据搬运和无 timeout waiter 注册路径
 *          不分配内存。首条唤醒消息先保存在等待体成员中，完成 timeout 裁决后才
 *          写入 output。
 * @note output 必须活到 await 完成，等待期间不得由其他线程或协程访问。
 */
template <BoundedValue T, size_t Capacity>
class BoundedRecvBatchToAwaitable
    : public TimeoutSupport<BoundedRecvBatchToAwaitable<T, Capacity>>
{
public:
    BoundedRecvBatchToAwaitable(const BoundedRecvBatchToAwaitable&) = delete;
    BoundedRecvBatchToAwaitable& operator=(
        const BoundedRecvBatchToAwaitable&) = delete;
    BoundedRecvBatchToAwaitable(BoundedRecvBatchToAwaitable&&) noexcept = default;
    BoundedRecvBatchToAwaitable& operator=(
        BoundedRecvBatchToAwaitable&&) noexcept = default;

    /**
     * @brief 尝试立即填充 output。
     *
     * @return 已取得消息或 output 为空时返回 true；需要等待消息时返回 false。
     */
    bool await_ready() noexcept;

    /**
     * @brief 空通道时注册唯一 consumer waiter。
     *
     * @tparam Promise 调用方协程 promise 类型。
     * @param handle 当前调用方的协程句柄。
     * @return 需要等待生产者提供消息时返回 true；已同步完成或失败时
     *         返回 false。
     *
     * @note 该函数只挂起协程，不阻塞调度器线程。
     */
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) noexcept;

    /**
     * @brief 返回实际搬运数量。
     *
     * @return 成功返回 [0, output.size()]；关闭、超时、构造 OOM/容量非法或同侧
     *         waiter 已占用返回对应 IOError。
     *
     * @note timeout、关闭或注册失败返回前不会修改 output。
     */
    std::expected<size_t, IOError> await_resume() noexcept;

private:
    friend class BoundedChannel<T, Capacity>;
    friend struct WithTimeout<BoundedRecvBatchToAwaitable<T, Capacity>>;

    BoundedRecvBatchToAwaitable(BoundedChannel<T, Capacity>* channel,
                                std::span<T> output) noexcept
        : m_output(output), m_channel(channel) {}

    bool tryReceiveNow() noexcept;
    void markTimeout() noexcept;
    void bindTimeoutTimer(const TimeoutTimer::ptr& timer) noexcept
    {
        m_timeoutTimer = timer;
    }

    std::optional<T> m_waiterReady;
    std::span<T> m_output;
    TimeoutTimer::ptr m_timeoutTimer;
    BoundedChannel<T, Capacity>* m_channel;
    BoundedChannelWaiter<T>* m_waiter = nullptr;
    uint64_t m_waiterGeneration = 0;
    size_t m_readyCount = 0;
    uint32_t m_registrationSystemError = 0;
    bool m_ready = false;
    bool m_timedOut = false;
    bool m_registrationFailed = false;
};

/**
 * @brief 固定容量、线程安全的 SPSC 异步通道。
 * @tparam T 元素类型，要求可移动且移动构造和析构不抛异常。
 * @tparam Capacity 编译期容量；默认 std::dynamic_extent 表示由构造参数决定。
 *
 * @details
 * - 动态容量会向上取整为不小于 2 的 2 的幂；编译期容量必须是合法的 2 次幂。
 * - trySend() / tryRecv() 不等待 Full/Empty 条件；单侧逻辑操作必须串行。
 * - send() / recv() / recvBatch() 满或空时只挂起协程，不阻塞底层线程。
 * - 单生产者与单消费者之间保持严格 FIFO。
 *
 * @note 通道是身份对象，不可复制或移动。通道必须存活到所有挂起操作完成或超时。
 * @note trySend(T&&) 只有在成功抢到 slot 后才移动参数；失败时参数保持未移动。
 *       需要失败后拿回值重试时应使用 trySend()。co_await send() 的值保存在 coroutine
 *       awaiter 中，超时或关闭时不会归还。
 */
template <BoundedValue T, size_t Capacity>
    requires detail::ValidBoundedChannelCapacity<Capacity>
class BoundedChannel
{
private:
    static constexpr bool kUsesStaticCapacity =
        Capacity != std::dynamic_extent;

public:
    static_assert(BoundedValue<T>, "BoundedChannel requires a movable T");
    static_assert(
        !kUsesStaticCapacity ||
            (Capacity >= 2 && std::has_single_bit(Capacity) &&
             Capacity <= (size_t{1} <<
                 (std::numeric_limits<size_t>::digits - 1U))),
        "static BoundedChannel capacity must be a safe power of two >= 2");

    /**
     * @brief 构造有界通道。
     *
     * @param capacity 期望容量；小于等于 2 时取 2，其余向上取整为 2 的幂；
     *                 超出游标安全半区间或槽位数组字节上限时构造失败。
     *
     * @details 使用 new(std::nothrow) 分配槽位；失败原因通过 error() 返回。
     *
     * @note 仅动态容量 specialization 提供该构造函数。
     */
    explicit BoundedChannel(size_t capacity) noexcept
        requires (!kUsesStaticCapacity)
    {
        const size_t normalized = normalizeCapacity(capacity);
        if (normalized == 0) {
            m_error = RingError::kCapacityTooLarge;
            return;
        }
        Slot* const slots = new (std::nothrow) Slot[normalized];
        if (slots == nullptr) {
            m_error = RingError::kAllocationFailed;
            return;
        }
        m_slots.reset(slots);
        m_capacity = normalized;
        m_mask = normalized - 1;
    }

    /**
     * @brief 构造成员内持有槽位的编译期容量通道。
     *
     * @details 不分配槽位内存；容量由 Capacity 唯一确定。
     *
     * @note 仅编译期容量 specialization 提供该构造函数。
     */
    BoundedChannel() noexcept
        requires kUsesStaticCapacity
    {
    }

    /**
     * @brief 销毁 ring 中尚未消费的已发布消息并释放通道资源。
     *
     * @pre 不得再有并发调用方或挂起在该通道上的 awaitable。
     *
     * @note 析构过程不负责关闭通道或等待其他线程退出。
     */
    ~BoundedChannel() noexcept
    {
        clearWaiterQueue(m_recvWaiters);
        clearWaiterQueue(m_sendWaiters);
        if (!isValid()) {
            return;
        }
        const size_t head = m_head.load(std::memory_order_relaxed);
        const size_t tail = m_tail.load(std::memory_order_relaxed);
        const size_t pending = tail - head;
        const size_t ringCapacity = capacity();
        const size_t count = pending < ringCapacity ? pending : ringCapacity;
        for (size_t i = 0; i < count; ++i) {
            const size_t position = head + i;
            Slot& slot = slotAt(position & ringMask());
            std::destroy_at(slot.value());
        }
    }

    /// @brief 禁止复制构造；通道具有唯一身份。
    BoundedChannel(const BoundedChannel&) = delete;

    /// @brief 禁止复制赋值；通道具有唯一身份。
    BoundedChannel& operator=(const BoundedChannel&) = delete;

    /// @brief 禁止移动构造，避免使已注册 waiter 持有失效地址。
    BoundedChannel(BoundedChannel&&) = delete;

    /// @brief 禁止移动赋值，避免使已注册 waiter 持有失效地址。
    BoundedChannel& operator=(BoundedChannel&&) = delete;

    /**
     * @brief 尝试立即发送一条消息。
     *
     * @param value 待发送消息；只有发送成功时才会被移动。
     * @return true 表示发送成功；false 表示通道已关闭或当前已满。
     *
     * @note 该路径不使用 cursor CAS；失败只表示关闭或当前容量已满。
     */
    bool trySend(T&& value)
    {
        PendingWakes pendingWakes;
        const auto enqueueResult = ringEnqueue(std::move(value));
        const bool sent = enqueueResult == RingEnqueueResult::kPublished;
        if (sent) {
            if (isClosed()) {
                finishCloseAfterPublication(pendingWakes);
            } else if (m_recvWaiterPathUsed.load(std::memory_order_seq_cst)) {
                wakeOneConsumerIfAny(pendingWakes);
            }
        } else if (enqueueResult == RingEnqueueResult::kClosedAfterPublishing) {
            finishCloseAfterPublication(pendingWakes);
        }
        pendingWakes.wakeAll();
        return sent;
    }

    /**
     * @brief 复制并尝试立即发送一条消息。
     *
     * @param value 待复制消息。
     * @return true 表示发送成功；false 表示通道已关闭或当前已满。
     *
     * @note 仅为不抛复制构造类型提供；先创建本地副本，再复用移动发送路径。
     */
    bool trySend(const T& value) requires std::is_nothrow_copy_constructible_v<T>
    {
        T copy = value;
        return trySend(std::move(copy));
    }

    /**
     * @brief 异步发送一条消息。
     *
     * @param value 待发送消息；会移动进等待体。
     * @return 可 co_await 的等待体；关闭返回 IOError(kClosed, 0)，超时返回 IOError(kTimeout, 0)。
     *
     * @note 满时挂起协程而不阻塞调度器线程；通道必须在等待期间保持有效。
     */
    BoundedSendAwaitable<T, Capacity> send(T&& value);

    /**
     * @brief 尝试立即接收一条消息。
     *
     * @return 有消息时返回消息；为空时返回 std::nullopt。关闭不影响排空残留消息。
     *
     * @note 该路径不使用 cursor CAS；空结果只表示当前没有已发布消息。
     */
    std::optional<T> tryRecv()
    {
        PendingWakes pendingWakes;
        std::optional<T> value;
        if (ringDequeueTo([&value](T&& item) {
                value.emplace(std::move(item));
            })) {
            if (m_sendWaiterPathUsed.load(std::memory_order_seq_cst)) {
                drainOneSendWaiter(pendingWakes);
            }
            pendingWakes.wakeAll();
            return value;
        }
        pendingWakes.wakeAll();
        return std::nullopt;
    }

    /**
     * @brief 异步接收一条消息。
     *
     * @return 可 co_await 的等待体；关闭且已排空时返回 IOError(kClosed, 0)，超时返回 IOError(kTimeout, 0)。
     *
     * @note 空时挂起协程而不阻塞调度器线程；通道必须在等待期间保持有效。
     */
    BoundedRecvAwaitable<T, Capacity> recv();

    /**
     * @brief 异步批量接收消息。
     *
     * @param count 单次最多接收的消息数。
     * @return 可 co_await 的等待体；至少收到一条后尽量补齐至 count 条。
     *
     * @note count 为 0 时立即返回空批次；返回 vector 的默认 allocator OOM 不经
     *       IOError。要求显式可恢复、无分配时使用 recvBatchTo(std::span<T>)。
     */
    BoundedRecvBatchAwaitable<T, Capacity> recvBatch(size_t count);

    /**
     * @brief 异步把消息移动到调用方已构造缓冲区。
     *
     * @param output 接收目标；空 span 立即成功返回 0。
     * @return 数据搬运和无 timeout waiter 注册路径不分配内存的等待体。
     *
     * @note output 必须活到 await 完成，等待期间不得由其他线程或协程访问。
     */
    [[nodiscard]] BoundedRecvBatchToAwaitable<T, Capacity> recvBatchTo(
        std::span<T> output) noexcept
        requires std::is_nothrow_move_assignable_v<T>;

    /**
     * @brief 尝试把消息移动到调用方已构造缓冲区。
     *
     * @param output 接收目标；空 span 不消费消息并返回 0。
     * @return 实际搬运数量，范围为 [0, output.size()]。
     *
     * @note 仅唯一 consumer 可调用；路径不分配，整批结束后只推进一次发送 waiter。
     */
    [[nodiscard]] size_t tryRecvBatch(std::span<T> output) noexcept
        requires std::is_nothrow_move_assignable_v<T>
    {
        if (output.empty()) {
            return 0;
        }

        PendingWakes pendingWakes;
        size_t received = 0;
        while (received < output.size()) {
            const bool dequeued = ringDequeueTo([&output, received](T&& value) noexcept {
                output[received] = std::move(value);
            });
            if (!dequeued) {
                break;
            }
            ++received;
        }
        if (received != 0 &&
            m_sendWaiterPathUsed.load(std::memory_order_seq_cst)) {
            drainOneSendWaiter(pendingWakes);
        }
        pendingWakes.wakeAll();
        return received;
    }

    /**
     * @brief 尝试批量接收消息。
     *
     * @param count 单次最多接收的消息数；0 返回空 vector。
     * @return 收到至少一条消息时返回消息批次；否则返回 std::nullopt。
     *
     * @note 返回 vector 的 convenience 路径可能分配，默认 allocator OOM 不经
     *       返回值传播；要求显式可恢复、无分配时使用 tryRecvBatch(std::span<T>)。
     */
    std::optional<std::vector<T>> tryRecvBatch(size_t count)
    {
        if (count == 0) {
            return std::vector<T>{};
        }
        if (!hasReadySlot()) {
            return std::nullopt;
        }

        std::vector<T> values;
        values.reserve(count);
        auto first = tryRecv();
        if (!first.has_value()) {
            return std::nullopt;
        }

        values.push_back(std::move(*first));
        while (values.size() < count) {
            auto value = tryRecv();
            if (!value.has_value()) {
                break;
            }
            values.push_back(std::move(*value));
        }
        return values;
    }

    /**
     * @brief 关闭通道并唤醒所有等待者。
     *
     * @details 关闭状态通过原子变量发布，可与发送、接收及其他 close() 调用并发执行。
     *
     * @note 操作幂等；关闭后发送失败，接收仍会先排空 ring 中的残留消息。
     */
    void close() noexcept
    {
        if (m_closed.exchange(true, std::memory_order_seq_cst)) {
            return;
        }
        PendingWakes pendingWakes;
        if (!hasPublishingSlot()) {
            wakeAllRecvWaiters(pendingWakes);
        }
        wakeAllSendWaiters(pendingWakes);
        // 从这里开始只访问栈上 wake batch；同步恢复即使销毁 channel 也不会 UAF。
        pendingWakes.wakeAll();
    }

    /**
     * @brief 查询通道是否已关闭。
     *
     * @return true 表示已关闭。
     *
     * @note 返回调用时刻的原子快照，可与其他通道操作并发调用。
     */
    bool isClosed() const noexcept
    {
        return m_closed.load(std::memory_order_acquire);
    }

    /**
     * @brief 返回构造状态。
     *
     * @return kNone 表示槽位可用；动态分配失败或容量越界返回对应 RingError。
     */
    [[nodiscard]] RingError error() const noexcept
    {
        return m_error;
    }

    /**
     * @brief 返回实际生效容量。
     *
     * @return 取整后的 2 的幂容量。
     *
     * @note 容量在构造后不再变化，可由任意线程读取。
     */
    size_t capacity() const noexcept
    {
        if constexpr (kUsesStaticCapacity) {
            return Capacity;
        } else {
            return m_capacity;
        }
    }

    /**
     * @brief 返回 ring 中的近似消息数。
     *
     * @return 仅供诊断，不可用作同步条件。
     */
    size_t size() const noexcept
    {
        const size_t tail = m_tail.load(std::memory_order_relaxed);
        const size_t head = m_head.load(std::memory_order_relaxed);
        const size_t count = tail - head;
        const size_t ringCapacity = capacity();
        return count < ringCapacity ? count : ringCapacity;
    }

    /**
     * @brief 近似检查 ring 是否为空。
     *
     * @return 仅供诊断，不可用作同步条件。
     */
    bool empty() const noexcept
    {
        return size() == 0;
    }

    /**
     * @brief 近似检查 ring 是否已满。
     *
     * @return 仅供诊断，不可用作同步条件。
     */
    bool full() const noexcept
    {
        return !isValid() || size() >= capacity();
    }

private:
    // AArch64 的隔离粒度是 128B，其他架构是 64B。head/tail 分行可避免
    // 诊断读取及 waiter helping 让两侧本地 cursor 发生伪共享。
    enum class SlotState : uint8_t {
        kEmpty,
        kPublishing,
        kReady,
    };

    enum class RingEnqueueResult : uint8_t {
        kFull,
        kClosed,
        kClosedAfterPublishing,
        kPublished,
    };

    struct Slot
    {
        alignas(T) std::byte storage[sizeof(T)];
        std::atomic<SlotState> ready{SlotState::kEmpty};

        /**
         * @brief 获取尚未开始 T 生命周期的原始存储地址。
         *
         * @return 仅供 construct_at 使用的对齐地址。
         */
        T* storageAddress() noexcept
        {
            return reinterpret_cast<T*>(storage);
        }

        /**
         * @brief 获取已完成构造的 T 对象地址。
         *
         * @return 已开始生命周期的 T 对象地址。
         */
        T* value() noexcept
        {
            return std::launder(reinterpret_cast<T*>(storage));
        }
    };

    static constexpr size_t kMaxCursorCapacity =
        size_t{1} << (std::numeric_limits<size_t>::digits - 1U);
    static constexpr size_t kMaxArrayCapacity = std::bit_floor(
        std::numeric_limits<size_t>::max() / sizeof(Slot));
    static constexpr size_t kMaxRingCapacity =
        kMaxCursorCapacity < kMaxArrayCapacity
        ? kMaxCursorCapacity
        : kMaxArrayCapacity;

    static constexpr size_t kStoredStaticCapacity =
        kUsesStaticCapacity ? Capacity : 0;
    using SlotStorage = std::conditional_t<
        kUsesStaticCapacity,
        std::array<Slot, kStoredStaticCapacity>,
        std::unique_ptr<Slot[]>>;

    [[nodiscard]] bool isValid() const noexcept
    {
        if constexpr (kUsesStaticCapacity) {
            return true;
        } else {
            return m_slots != nullptr;
        }
    }

    [[nodiscard]] IOError constructionError() const noexcept
    {
        return m_error == RingError::kAllocationFailed
            ? IOError(kernel::kOutOfMemory, 0)
            : IOError(kernel::kParamInvalid, 0);
    }

    [[nodiscard]] size_t ringMask() const noexcept
    {
        if constexpr (kUsesStaticCapacity) {
            return Capacity - 1;
        } else {
            return m_mask;
        }
    }

    [[nodiscard]] Slot& slotAt(size_t index) noexcept
    {
        if constexpr (kUsesStaticCapacity) {
            return m_slots[index];
        } else {
            return m_slots.get()[index];
        }
    }

    [[nodiscard]] const Slot& slotAt(size_t index) const noexcept
    {
        if constexpr (kUsesStaticCapacity) {
            return m_slots[index];
        } else {
            return m_slots.get()[index];
        }
    }

    using Waiter = BoundedChannelWaiter<T>;

    /**
     * @brief 固定 waiter 槽的 move-only 推进引用。
     * @details queue 发布持有一个 pin；dequeue 将该 pin 无缝转交给 lease。
     *          await_resume 只有在所有 lease 释放后才允许复用槽内非原子字段。
     */
    class WaiterLease
    {
    public:
        WaiterLease() noexcept = default;
        WaiterLease(const WaiterLease&) = delete;
        WaiterLease& operator=(const WaiterLease&) = delete;

        WaiterLease(WaiterLease&& other) noexcept
            : m_waiter(std::exchange(other.m_waiter, nullptr))
        {
        }

        WaiterLease& operator=(WaiterLease&& other) noexcept
        {
            if (this != &other) {
                reset();
                m_waiter = std::exchange(other.m_waiter, nullptr);
            }
            return *this;
        }

        ~WaiterLease() noexcept
        {
            reset();
        }

        /**
         * @brief 取得当前固定 waiter 槽的非拥有指针。
         *
         * @return 当前 waiter 指针；lease 为空时返回 nullptr。
         */
        [[nodiscard]] Waiter* get() const noexcept
        {
            return m_waiter;
        }

        /**
         * @brief 释放当前推进 pin。
         */
        void reset() noexcept
        {
            if (m_waiter == nullptr) {
                return;
            }
            [[maybe_unused]] const size_t previousPins =
                m_waiter->pins.fetch_sub(1, std::memory_order_release);
            m_waiter = nullptr;
        }

    private:
        friend class BoundedChannel<T, Capacity>;

        struct AdoptQueuePin {};

        explicit WaiterLease(Waiter* waiter, AdoptQueuePin) noexcept
            : m_waiter(waiter)
        {
        }

        Waiter* m_waiter = nullptr;
    };

    /** @brief 保存单侧至多一个延迟唤醒；SPSC 每侧只允许一个 live waiter。 */
    struct PendingWake
    {
        TimeoutTimer::ptr timeoutWinner;
        std::optional<Waker> waker;

        void setWaker(Waker waiterWaker) noexcept
        {
            waker.emplace(std::move(waiterWaker));
        }

        void setTimeoutWinner(const TimeoutTimer::ptr& timer) noexcept
        {
            timeoutWinner = timer;
        }

        void wake() noexcept
        {
            TimeoutTimer::ptr timer = std::move(timeoutWinner);
            std::optional<Waker> waiterWaker = std::move(waker);
            if (timer) {
                timer->wakeTimeoutWinner();
            }
            if (waiterWaker.has_value()) {
                waiterWaker->wakeUp();
            }
        }
    };

    /** @brief 收集两侧唤醒，确保实际恢复发生在最后一次 channel 访问之后。 */
    struct PendingWakes
    {
        PendingWake send;
        PendingWake recv;

        void wakeAll() noexcept
        {
            send.wake();
            recv.wake();
        }
    };

    /**
     * @brief SPSC 单侧串行调用约束下的单 waiter intrusive slot。
     * @details 原子指针只负责可发现性；对应固定 waiter 的 pins 持有队列引用。
     */
    struct WaiterQueue
    {
        std::atomic<Waiter*> waiter{nullptr};
    };

    /**
     * @brief 在 await_suspend 完成 arming 后唤醒 waiter 关联的协程。
     *
     * @param waiter 已完成状态发布的等待体。
     * @param pendingWake 延迟到最后一次 channel 访问之后执行的唤醒槽。
     *
     * @note arming 窗口内只登记 pending wake；实际恢复请求全局至多提交一次。
     */
    void prepareWaiterWake(Waiter& waiter, PendingWake& pendingWake) noexcept
    {
        auto phase = waiter.wakePhase.load(std::memory_order_acquire);
        for (;;) {
            if (phase == BoundedWaiterWakePhase::kArming) {
                if (waiter.wakePhase.compare_exchange_weak(
                        phase,
                        BoundedWaiterWakePhase::kPendingWake,
                        std::memory_order_release,
                        std::memory_order_acquire)) {
                    return;
                }
                continue;
            }
            if (phase == BoundedWaiterWakePhase::kArmed) {
                if (waiter.wakePhase.compare_exchange_weak(
                        phase,
                        BoundedWaiterWakePhase::kWakeIssued,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    Waker waker = std::move(waiter.waker);
                    waiter.wakeIssued.store(true, std::memory_order_release);
                    pendingWake.setWaker(std::move(waker));
                    return;
                }
                continue;
            }
            return;
        }
    }

    /**
     * @brief 尝试认领 waiter；队列 helper 会先登记一次重试请求。
     *
     * @param waiter 已从对应等待队列取得或由注册方直接推进的等待体。
     * @param requestRetry true 表示调用方已从队列移除该 waiter，必须先通知当前 owner。
     * @return 成功完成 kWaiting -> kFulfilling 转换时返回 true。
     *
     * @note retry 必须先于 CAS 发布：若 owner 未观察到 generation 变化，随后 CAS 必然能在
     *       owner 发布 kWaiting 后认领 waiter，因此不能把该顺序改回“CAS 失败后再登记”。
     */
    bool claimWaiter(Waiter* waiter, bool requestRetry) noexcept
    {
        if (waiter == nullptr) {
            return false;
        }
        if (requestRetry) {
            [[maybe_unused]] const uint64_t previousRetryRequest =
                waiter->retryRequested.fetch_add(1, std::memory_order_release);
        }
        BoundedWaiterState expected = BoundedWaiterState::kWaiting;
        return waiter->state.compare_exchange_strong(
            expected,
            BoundedWaiterState::kFulfilling,
            std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    /**
     * @brief 保证 waiter 在指定队列中至多保留一个可发现入口。
     *
     * @param waiters 目标单 waiter 队列。
     * @param waiter 待发布的固定 waiter 槽。
     * @return waiter 已在队列中或本次发布成功时返回 true；队列被其他
     *         waiter 占用时返回 false。
     */
    bool enqueueWaiter(WaiterQueue& waiters, Waiter* waiter) noexcept
    {
        if (waiter == nullptr) {
            return false;
        }
        bool expected = false;
        if (!waiter->queued.compare_exchange_strong(expected,
                                                    true,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_acquire)) {
            return true;
        }
        [[maybe_unused]] const size_t previousPins =
            waiter->pins.fetch_add(1, std::memory_order_relaxed);
        Waiter* expectedWaiter = nullptr;
        if (waiters.waiter.compare_exchange_strong(expectedWaiter,
                                                   waiter,
                                                   std::memory_order_release,
                                                   std::memory_order_acquire)) {
            return true;
        }
        waiter->queued.store(false, std::memory_order_release);
        [[maybe_unused]] const size_t pinsBeforeRollback =
            waiter->pins.fetch_sub(1, std::memory_order_release);
        return false;
    }

    /**
     * @brief 从指定队列移除一个有效入口，并把队列 pin 转交给推进 lease。
     *
     * @param waiters 目标单 waiter 队列。
     * @param lease 接收推进 pin 的 lease。
     * @return 成功取得有效 waiter 时返回 true；队列为空或入口已失效时
     *         返回 false。
     */
    bool tryDequeueWaiter(WaiterQueue& waiters, WaiterLease& lease) noexcept
    {
        lease.reset();
        Waiter* const queuedWaiter =
            waiters.waiter.exchange(nullptr, std::memory_order_acq_rel);
        if (queuedWaiter == nullptr) {
            return false;
        }
        lease = WaiterLease(queuedWaiter,
                            typename WaiterLease::AdoptQueuePin{});
        if (!queuedWaiter->queued.exchange(false, std::memory_order_acq_rel)) {
            lease.reset();
            return false;
        }
#ifdef GALAY_SPSC_BOUNDED_DEQUEUE_TEST_POINT
        GALAY_SPSC_BOUNDED_DEQUEUE_TEST_POINT(queuedWaiter);
#endif
        return true;
    }

    /**
     * @brief 终态 owner 尝试摘除 waiter 并释放队列 pin。
     *
     * @param waiters waiter 所属的单 waiter 队列。
     * @param waiter 进入终态的固定 waiter 槽。
     */
    void removeQueuedWaiter(WaiterQueue& waiters, Waiter* waiter) noexcept
    {
        if (waiter == nullptr) {
            return;
        }
        Waiter* expectedWaiter = waiter;
        if (!waiters.waiter.compare_exchange_strong(expectedWaiter,
                                                    nullptr,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_acquire)) {
            return;
        }
        if (waiter->queued.exchange(false, std::memory_order_acq_rel)) {
            [[maybe_unused]] const size_t previousPins =
                waiter->pins.fetch_sub(1, std::memory_order_release);
        }
    }

    /**
     * @brief 析构前清空无并发访问的 waiter slot，并释放队列 pin。
     *
     * @param waiters 待清空的单 waiter 队列。
     */
    void clearWaiterQueue(WaiterQueue& waiters) noexcept
    {
        Waiter* const queuedWaiter =
            waiters.waiter.exchange(nullptr, std::memory_order_relaxed);
        if (queuedWaiter == nullptr) {
            return;
        }
        if (queuedWaiter->queued.exchange(false, std::memory_order_relaxed)) {
            [[maybe_unused]] const size_t previousPins =
                queuedWaiter->pins.fetch_sub(1, std::memory_order_relaxed);
        }
    }

    /**
     * @brief 在 await_resume 中安全回收一个 terminal waiter generation。
     *
     * @param waiters waiter 所属的单 waiter 队列。
     * @param waiter 待回收的固定 waiter 槽。
     * @param waiterGeneration 当前 awaitable 注册时获得的 generation。
     * @return generation 匹配且固定槽已重新发布为 kIdle 时返回 true。
     *
     * @note terminal state 会阻止新认领；摘队列后等待的只是已持 pin 推进方的
     *       有限收尾窗口。所有完成方必须在实际 wake 前释放 lease。
     */
    bool reclaimWaiter(WaiterQueue& waiters,
                       Waiter& waiter,
                       uint64_t waiterGeneration) noexcept
    {
        if (waiter.generation.load(std::memory_order_acquire) != waiterGeneration) {
            return false;
        }

        removeQueuedWaiter(waiters, &waiter);
        while (waiter.pins.load(std::memory_order_acquire) != 0) {
#ifdef GALAY_SPSC_BOUNDED_RECLAIM_TEST_POINT
            GALAY_SPSC_BOUNDED_RECLAIM_TEST_POINT(&waiter);
#endif
            detail::boundedChannelCpuPause();
        }

        BoundedWaiterState terminal = waiter.state.load(std::memory_order_acquire);
        if (waiter.generation.load(std::memory_order_acquire) != waiterGeneration ||
            terminal == BoundedWaiterState::kIdle ||
            terminal == BoundedWaiterState::kRegistering ||
            terminal == BoundedWaiterState::kWaiting ||
            terminal == BoundedWaiterState::kFulfilling ||
            !waiter.state.compare_exchange_strong(
                terminal,
                BoundedWaiterState::kReclaiming,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return false;
        }

        waiter.waker = Waker();
        waiter.timeoutTimer.reset();
        waiter.sendValue = nullptr;
        waiter.recvValue = nullptr;
        waiter.retryRequested.store(0, std::memory_order_relaxed);
        waiter.wakePhase.store(BoundedWaiterWakePhase::kArming,
                               std::memory_order_relaxed);
        waiter.wakeIssued.store(false, std::memory_order_relaxed);
        waiter.queued.store(false, std::memory_order_relaxed);
        waiter.state.store(BoundedWaiterState::kIdle, std::memory_order_release);
        return true;
    }

    void synchronizeRecvWaiterPath() noexcept
    {
        if (!isValid()) {
            return;
        }
        const size_t position = m_head.load(std::memory_order_relaxed);
        // fallback 路径与 SC ready.store 组成 Dekker 握手；native 路径在首次
        // waiter 发布后先执行 process-wide barrier，再用该 load 复核 slot。
        [[maybe_unused]] const SlotState published =
            slotAt(position & ringMask()).ready.load(std::memory_order_seq_cst);
    }

    void synchronizeSendWaiterPath() noexcept
    {
        if (!isValid()) {
            return;
        }
        const size_t position = m_tail.load(std::memory_order_relaxed);
        // fallback 路径与 SC ready.store 组成 Dekker 握手；native 路径在首次
        // waiter 发布后先执行 process-wide barrier，再用该 load 复核 slot。
        [[maybe_unused]] const SlotState released =
            slotAt(position & ringMask()).ready.load(std::memory_order_seq_cst);
    }

    std::optional<uint32_t> firstWaiterBarrierError(bool firstUse) noexcept
    {
        if (!firstUse || !m_useAsymmetricBarrier) {
            return std::nullopt;
        }
        auto barrier = kernel::detail::asymmetricHeavyBarrier();
        if (barrier) {
            return std::nullopt;
        }
        return static_cast<uint32_t>(barrier.error().systemError);
    }

    /**
     * @brief close 冷路径扫描是否存在已声明但尚未完成构造的发送。
     *
     * @return 存在 kPublishing 槽位时返回 true。
     */
    bool hasPublishingSlot() const noexcept
    {
        if (!isValid()) {
            return false;
        }
        for (size_t index = 0; index < capacity(); ++index) {
            const Slot& slot = slotAt(index);
            if (slot.ready.load(std::memory_order_seq_cst) == SlotState::kPublishing) {
                return true;
            }
        }
        return false;
    }

    /**
     * @brief 判断接收侧是否可以观察到通道关闭。
     *
     * @return 通道已关闭且当前 head 不处于发布窗口时返回 true。
     */
    bool isReceiveClosed() const noexcept
    {
        if (!m_closed.load(std::memory_order_acquire)) {
            return false;
        }
        if (!isValid()) {
            return true;
        }
        const size_t position = m_head.load(std::memory_order_relaxed);
        return slotAt(position & ringMask()).ready.load(std::memory_order_acquire) ==
            SlotState::kEmpty;
    }

    /**
     * @brief 检查当前 consumer head 是否已发布，供批量路径在分配前探测。
     *
     * @return 当前 head 槽位为 kReady 时返回 true。
     */
    bool hasReadySlot() const noexcept
    {
        if (!isValid()) {
            return false;
        }
        const size_t position = m_head.load(std::memory_order_relaxed);
        return slotAt(position & ringMask()).ready.load(std::memory_order_acquire) ==
            SlotState::kReady;
    }

    /**
     * @brief 将请求容量规范化为 ring 所需的 2 的幂。
     * @param capacity 调用方请求的容量。
     * @return 正常请求向上取整后的 2 的幂；超出安全半区间时返回 0。
     */
    static size_t normalizeCapacity(size_t capacity) noexcept
    {
        if (kMaxRingCapacity < 2) {
            return 0;
        }
        if (capacity <= 2) {
            return 2;
        }
        // head/tail 差值按无符号半区间解释；同时避免 bit_ceil 最高位溢出。
        if (capacity > kMaxRingCapacity) {
            return 0;
        }
        return std::bit_ceil(capacity);
    }

    /**
     * @brief 尝试向 SPSC ring 发布一条消息。
     * @param value 待发布消息；仅在确认存在空闲 slot 后移动。
     * @return 发布成功返回 true；当前 ring 已满时返回 false。
     * @note 同一时刻只能有一个逻辑发送操作；waiter helping 可切换物理执行线程，
     *       因此 cursor 保持 atomic，但不执行 CAS。
     */
    RingEnqueueResult ringEnqueue(T&& value) noexcept
    {
        if (!isValid()) {
            return RingEnqueueResult::kFull;
        }
        if (m_closed.load(std::memory_order_acquire)) {
            return RingEnqueueResult::kClosed;
        }
        const size_t position = m_tail.load(std::memory_order_relaxed);
        Slot& slot = slotAt(position & ringMask());
        if (slot.ready.load(std::memory_order_acquire) != SlotState::kEmpty) {
            return RingEnqueueResult::kFull;
        }

        // 成功发送以该 SC 发布为线性化点；close 的 SC 发布与随后全 slot 扫描
        // 组成 Dekker 握手，保证 close 与发送至少一方观察到另一方。
        slot.ready.store(SlotState::kPublishing, std::memory_order_seq_cst);
        if (m_closed.load(std::memory_order_seq_cst)) {
            slot.ready.store(SlotState::kEmpty, std::memory_order_seq_cst);
            return RingEnqueueResult::kClosedAfterPublishing;
        }

        [[maybe_unused]] T* const stored =
            std::construct_at(slot.storageAddress(), std::move(value));
        // close 仍由 kPublishing 的 SC 发布仲裁；waiter 首次启用时
        // 的 process-wide barrier 允许稳定数据发布使用 release。
        if (m_useAsymmetricBarrier) {
            slot.ready.store(SlotState::kReady, std::memory_order_release);
            kernel::detail::asymmetricLightBarrier();
        } else {
            slot.ready.store(SlotState::kReady, std::memory_order_seq_cst);
        }
        m_tail.store(position + 1, std::memory_order_relaxed);
        return RingEnqueueResult::kPublished;
    }

    /**
     * @brief 尝试从 SPSC ring 消费一条消息。
     * @tparam Consume 接收 T&& 的不可抛出消费回调类型。
     * @param consume 在发现已发布消息后调用一次的消费回调。
     * @return 消费成功返回 true；当前 ring 为空时返回 false。
     * @note 同一时刻只能有一个逻辑接收操作；waiter helping 可切换物理执行线程，
     *       因此 cursor 保持 atomic，但不执行 CAS。
     */
    template <typename Consume>
    bool ringDequeueTo(Consume&& consume) noexcept
    {
        if (!isValid()) {
            return false;
        }
        const size_t position = m_head.load(std::memory_order_relaxed);
        Slot& slot = slotAt(position & ringMask());
        if (slot.ready.load(std::memory_order_acquire) != SlotState::kReady) {
            return false;
        }

        consume(std::move(*slot.value()));
        std::destroy_at(slot.value());
        if (m_useAsymmetricBarrier) {
            slot.ready.store(SlotState::kEmpty, std::memory_order_release);
            kernel::detail::asymmetricLightBarrier();
        } else {
            slot.ready.store(SlotState::kEmpty, std::memory_order_seq_cst);
        }
        m_head.store(position + 1, std::memory_order_relaxed);
        return true;
    }

    /**
     * @brief 尝试推进一个发送 waiter 的状态。
     * @param waiter 待推进的共享等待体；空指针视为未认领。
     * @param wake 完成、关闭或失败时是否立即唤醒关联协程。
     * @return kCompleted 表示等待体已结束，kWaiting 表示已重新排队，
     *         kNotClaimed 表示等待体已由其他并发路径处理。
     * @note ring 仍满时会把 waiter 恢复为 kWaiting 并重新入队。
     */
    BoundedWaiterProgress tryCompleteSendWaiter(Waiter* waiter,
                                                bool wake,
                                                PendingWakes& pendingWakes) noexcept
    {
        if (!waiter) {
            return BoundedWaiterProgress::kNotClaimed;
        }

        TimeoutTimer* const timeoutTimer = waiter->timeoutTimer.get();
        const bool retryPreannounced = wake && timeoutTimer != nullptr;
        if (retryPreannounced) {
            // 先于 timeout 状态观察发布 retry，使 abort 后的 SC 复核能区分：
            // peer 观察到 InFlight 时 owner 必须看到 retry；否则 peer 自行认领。
            [[maybe_unused]] const uint64_t previousRetryRequest =
                waiter->retryRequested.fetch_add(1, std::memory_order_seq_cst);
        }

retry_timeout_operation:
        bool timeoutOperationStarted = false;
        if (timeoutTimer != nullptr) {
            const auto start = timeoutTimer->tryBeginOperation();
            if (start == TimeoutTimer::OperationStart::kBusy) {
                const auto state = waiter->state.load(std::memory_order_acquire);
                if (state == BoundedWaiterState::kWaiting ||
                    state == BoundedWaiterState::kFulfilling) {
                    const bool requeued = enqueueWaiter(m_sendWaiters, waiter);
                    if (!requeued) {
                        // transaction owner 已观察 retryRequested，并负责最终重排或失败。
                    }
                }
                return BoundedWaiterProgress::kWaiting;
            }
            if (start == TimeoutTimer::OperationStart::kTimeoutWon) {
                BoundedWaiterState expected = BoundedWaiterState::kWaiting;
                if (!waiter->state.compare_exchange_strong(
                        expected,
                        BoundedWaiterState::kCancelled,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    // timeout winner 或当前 operation owner 会发布最终状态。
                }
                removeQueuedWaiter(m_sendWaiters, waiter);
                return BoundedWaiterProgress::kSkipped;
            }
            if (start == TimeoutTimer::OperationStart::kOperationWon) {
                removeQueuedWaiter(m_sendWaiters, waiter);
                return BoundedWaiterProgress::kSkipped;
            }
            timeoutOperationStarted = true;
        }

        if (!claimWaiter(waiter, wake && !retryPreannounced)) {
            if (timeoutOperationStarted) {
                const auto aborted = timeoutTimer->abortOperation();
                if (aborted == TimeoutTimer::OperationAbort::kTimeoutWon) {
                    waiter->state.store(BoundedWaiterState::kCancelled,
                                        std::memory_order_release);
                    removeQueuedWaiter(m_sendWaiters, waiter);
                    pendingWakes.send.setTimeoutWinner(waiter->timeoutTimer);
                    return BoundedWaiterProgress::kSkipped;
                }
                if (aborted == TimeoutTimer::OperationAbort::kCompleted) {
                    removeQueuedWaiter(m_sendWaiters, waiter);
                    return BoundedWaiterProgress::kSkipped;
                }
            }
            const auto state = waiter->state.load(std::memory_order_acquire);
            if (state != BoundedWaiterState::kWaiting &&
                state != BoundedWaiterState::kFulfilling) {
                removeQueuedWaiter(m_sendWaiters, waiter);
                return BoundedWaiterProgress::kSkipped;
            }
            return BoundedWaiterProgress::kNotClaimed;
        }
        for (;;) {
            const uint64_t retryEpoch =
                waiter->retryRequested.load(std::memory_order_seq_cst);

            if (isClosed()) {
                waiter->state.store(BoundedWaiterState::kClosed, std::memory_order_release);
                const bool completionCommitted =
                    !timeoutOperationStarted || timeoutTimer->commitOperation();
                if (!completionCommitted) {
                    waiter->state.store(BoundedWaiterState::kFailed,
                                        std::memory_order_release);
                }
                removeQueuedWaiter(m_sendWaiters, waiter);
                if (wake) {
                    prepareWaiterWake(*waiter, pendingWakes.send);
                }
                return BoundedWaiterProgress::kCompleted;
            }

            RingEnqueueResult enqueueResult = RingEnqueueResult::kFull;
            if (waiter->sendValue != nullptr) {
                enqueueResult = ringEnqueue(std::move(*waiter->sendValue));
            }
            if (enqueueResult == RingEnqueueResult::kPublished) {
                waiter->state.store(BoundedWaiterState::kFulfilled,
                                    std::memory_order_release);
                const bool completionCommitted =
                    !timeoutOperationStarted || timeoutTimer->commitOperation();
                if (!completionCommitted) {
                    waiter->state.store(BoundedWaiterState::kFailed,
                                        std::memory_order_release);
                }
                removeQueuedWaiter(m_sendWaiters, waiter);
                if (wake) {
                    prepareWaiterWake(*waiter, pendingWakes.send);
                }
                if (isClosed()) {
                    finishCloseAfterPublication(pendingWakes);
                }
                return BoundedWaiterProgress::kCompleted;
            }
            if (enqueueResult == RingEnqueueResult::kClosed ||
                enqueueResult == RingEnqueueResult::kClosedAfterPublishing) {
                waiter->state.store(BoundedWaiterState::kClosed, std::memory_order_release);
                const bool completionCommitted =
                    !timeoutOperationStarted || timeoutTimer->commitOperation();
                if (!completionCommitted) {
                    waiter->state.store(BoundedWaiterState::kFailed,
                                        std::memory_order_release);
                }
                removeQueuedWaiter(m_sendWaiters, waiter);
                if (wake) {
                    prepareWaiterWake(*waiter, pendingWakes.send);
                }
                if (enqueueResult == RingEnqueueResult::kClosedAfterPublishing) {
                    finishCloseAfterPublication(pendingWakes);
                }
                return BoundedWaiterProgress::kCompleted;
            }

            // 先保证唯一队列入口存在，再发布 kWaiting；enqueue 失败时 owner 仍独占状态。
            if (!enqueueWaiter(m_sendWaiters, waiter)) {
                waiter->state.store(BoundedWaiterState::kFailed, std::memory_order_release);
                const bool completionCommitted =
                    !timeoutOperationStarted || timeoutTimer->commitOperation();
                if (!completionCommitted) {
                    // kFailed 已是唯一可传播的终态，不再覆盖。
                }
                removeQueuedWaiter(m_sendWaiters, waiter);
                if (wake) {
                    prepareWaiterWake(*waiter, pendingWakes.send);
                }
                return BoundedWaiterProgress::kCompleted;
            }
            waiter->state.store(BoundedWaiterState::kWaiting, std::memory_order_release);
            if (waiter->retryRequested.load(std::memory_order_seq_cst) == retryEpoch) {
                if (timeoutOperationStarted) {
#ifdef GALAY_SPSC_BOUNDED_TIMEOUT_ABORT_TEST_POINT
                    GALAY_SPSC_BOUNDED_TIMEOUT_ABORT_TEST_POINT(waiter);
#endif
                    const auto aborted = timeoutTimer->abortOperation();
                    if (aborted == TimeoutTimer::OperationAbort::kTimeoutWon) {
                        waiter->state.store(BoundedWaiterState::kCancelled,
                                            std::memory_order_release);
                        removeQueuedWaiter(m_sendWaiters, waiter);
                        pendingWakes.send.setTimeoutWinner(waiter->timeoutTimer);
                        return BoundedWaiterProgress::kSkipped;
                    }
                    if (aborted == TimeoutTimer::OperationAbort::kCompleted) {
                        removeQueuedWaiter(m_sendWaiters, waiter);
                        return BoundedWaiterProgress::kSkipped;
                    }
                    timeoutOperationStarted = false;
                    if (waiter->retryRequested.load(std::memory_order_seq_cst) !=
                        retryEpoch) {
                        goto retry_timeout_operation;
                    }
                }
                if (isClosed()) {
                    wakeAllSendWaiters(pendingWakes);
                }
                return BoundedWaiterProgress::kWaiting;
            }

            BoundedWaiterState expected = BoundedWaiterState::kWaiting;
            if (!waiter->state.compare_exchange_strong(
                    expected,
                    BoundedWaiterState::kFulfilling,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                if (timeoutOperationStarted) {
                    const auto aborted = timeoutTimer->abortOperation();
                    if (aborted == TimeoutTimer::OperationAbort::kTimeoutWon) {
                        waiter->state.store(BoundedWaiterState::kCancelled,
                                            std::memory_order_release);
                        removeQueuedWaiter(m_sendWaiters, waiter);
                        pendingWakes.send.setTimeoutWinner(waiter->timeoutTimer);
                        return BoundedWaiterProgress::kSkipped;
                    }
                    if (aborted == TimeoutTimer::OperationAbort::kCompleted) {
                        removeQueuedWaiter(m_sendWaiters, waiter);
                        return BoundedWaiterProgress::kSkipped;
                    }
                }
                return BoundedWaiterProgress::kNotClaimed;
            }
        }
    }

    /**
     * @brief 尝试推进一个接收 waiter 的状态。
     * @param waiter 待推进的共享等待体；空指针视为未认领。
     * @param wake 完成、关闭或失败时是否立即唤醒关联协程。
     * @return kCompleted 表示等待体已结束，kWaiting 表示已重新排队，
     *         kNotClaimed 表示等待体已由其他并发路径处理。
     * @note ring 仍空时会把 waiter 恢复为 kWaiting 并重新入队。
     */
    BoundedWaiterProgress tryCompleteRecvWaiter(Waiter* waiter,
                                                bool wake,
                                                PendingWakes& pendingWakes) noexcept
    {
        if (!waiter) {
            return BoundedWaiterProgress::kNotClaimed;
        }

        TimeoutTimer* const timeoutTimer = waiter->timeoutTimer.get();
        const bool retryPreannounced = wake && timeoutTimer != nullptr;
        if (retryPreannounced) {
            // 与发送侧相同：retry 必须先于 timeout 状态观察进入 SC 顺序。
            [[maybe_unused]] const uint64_t previousRetryRequest =
                waiter->retryRequested.fetch_add(1, std::memory_order_seq_cst);
        }

retry_timeout_operation:
        bool timeoutOperationStarted = false;
        if (timeoutTimer != nullptr) {
            const auto start = timeoutTimer->tryBeginOperation();
            if (start == TimeoutTimer::OperationStart::kBusy) {
                const auto state = waiter->state.load(std::memory_order_acquire);
                if (state == BoundedWaiterState::kWaiting ||
                    state == BoundedWaiterState::kFulfilling) {
                    const bool requeued = enqueueWaiter(m_recvWaiters, waiter);
                    if (!requeued) {
                        // transaction owner 已观察 retryRequested，并负责最终重排或失败。
                    }
                }
                return BoundedWaiterProgress::kWaiting;
            }
            if (start == TimeoutTimer::OperationStart::kTimeoutWon) {
                BoundedWaiterState expected = BoundedWaiterState::kWaiting;
                if (!waiter->state.compare_exchange_strong(
                        expected,
                        BoundedWaiterState::kCancelled,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    // timeout winner 或当前 operation owner 会发布最终状态。
                }
                removeQueuedWaiter(m_recvWaiters, waiter);
                return BoundedWaiterProgress::kSkipped;
            }
            if (start == TimeoutTimer::OperationStart::kOperationWon) {
                removeQueuedWaiter(m_recvWaiters, waiter);
                return BoundedWaiterProgress::kSkipped;
            }
            timeoutOperationStarted = true;
        }

        if (!claimWaiter(waiter, wake && !retryPreannounced)) {
            if (timeoutOperationStarted) {
                const auto aborted = timeoutTimer->abortOperation();
                if (aborted == TimeoutTimer::OperationAbort::kTimeoutWon) {
                    waiter->state.store(BoundedWaiterState::kCancelled,
                                        std::memory_order_release);
                    removeQueuedWaiter(m_recvWaiters, waiter);
                    pendingWakes.recv.setTimeoutWinner(waiter->timeoutTimer);
                    return BoundedWaiterProgress::kSkipped;
                }
                if (aborted == TimeoutTimer::OperationAbort::kCompleted) {
                    removeQueuedWaiter(m_recvWaiters, waiter);
                    return BoundedWaiterProgress::kSkipped;
                }
            }
            const auto state = waiter->state.load(std::memory_order_acquire);
            if (state != BoundedWaiterState::kWaiting &&
                state != BoundedWaiterState::kFulfilling) {
                removeQueuedWaiter(m_recvWaiters, waiter);
                return BoundedWaiterProgress::kSkipped;
            }
            return BoundedWaiterProgress::kNotClaimed;
        }
        for (;;) {
            const uint64_t retryEpoch =
                waiter->retryRequested.load(std::memory_order_seq_cst);

            const bool wakeOnly = waiter->recvValue == nullptr;
            const bool ready = wakeOnly
                ? hasReadySlot()
                : ringDequeueTo([&waiter](T&& value) {
                      waiter->recvValue->emplace(std::move(value));
                  });
            if (ready) {
                waiter->state.store(BoundedWaiterState::kFulfilled,
                                    std::memory_order_release);
                const bool completionCommitted =
                    !timeoutOperationStarted || timeoutTimer->commitOperation();
                if (!completionCommitted) {
                    waiter->state.store(BoundedWaiterState::kFailed,
                                        std::memory_order_release);
                }
                if (!wakeOnly) {
                    drainOneSendWaiter(pendingWakes);
                }
                removeQueuedWaiter(m_recvWaiters, waiter);
                if (wake) {
                    prepareWaiterWake(*waiter, pendingWakes.recv);
                }
                return BoundedWaiterProgress::kCompleted;
            }

            if (isReceiveClosed()) {
                waiter->state.store(BoundedWaiterState::kClosed, std::memory_order_release);
                const bool completionCommitted =
                    !timeoutOperationStarted || timeoutTimer->commitOperation();
                if (!completionCommitted) {
                    waiter->state.store(BoundedWaiterState::kFailed,
                                        std::memory_order_release);
                }
                removeQueuedWaiter(m_recvWaiters, waiter);
                if (wake) {
                    prepareWaiterWake(*waiter, pendingWakes.recv);
                }
                return BoundedWaiterProgress::kCompleted;
            }

            if (!enqueueWaiter(m_recvWaiters, waiter)) {
                waiter->state.store(BoundedWaiterState::kFailed, std::memory_order_release);
                const bool completionCommitted =
                    !timeoutOperationStarted || timeoutTimer->commitOperation();
                if (!completionCommitted) {
                    // kFailed 已是唯一可传播的终态，不再覆盖。
                }
                removeQueuedWaiter(m_recvWaiters, waiter);
                if (wake) {
                    prepareWaiterWake(*waiter, pendingWakes.recv);
                }
                return BoundedWaiterProgress::kCompleted;
            }
            waiter->state.store(BoundedWaiterState::kWaiting, std::memory_order_release);
            if (waiter->retryRequested.load(std::memory_order_seq_cst) == retryEpoch) {
                if (timeoutOperationStarted) {
#ifdef GALAY_SPSC_BOUNDED_TIMEOUT_ABORT_TEST_POINT
                    GALAY_SPSC_BOUNDED_TIMEOUT_ABORT_TEST_POINT(waiter);
#endif
                    const auto aborted = timeoutTimer->abortOperation();
                    if (aborted == TimeoutTimer::OperationAbort::kTimeoutWon) {
                        waiter->state.store(BoundedWaiterState::kCancelled,
                                            std::memory_order_release);
                        removeQueuedWaiter(m_recvWaiters, waiter);
                        pendingWakes.recv.setTimeoutWinner(waiter->timeoutTimer);
                        return BoundedWaiterProgress::kSkipped;
                    }
                    if (aborted == TimeoutTimer::OperationAbort::kCompleted) {
                        removeQueuedWaiter(m_recvWaiters, waiter);
                        return BoundedWaiterProgress::kSkipped;
                    }
                    timeoutOperationStarted = false;
                    if (waiter->retryRequested.load(std::memory_order_seq_cst) !=
                        retryEpoch) {
                        goto retry_timeout_operation;
                    }
                }
                if (isReceiveClosed()) {
                    wakeAllRecvWaiters(pendingWakes);
                }
                return BoundedWaiterProgress::kWaiting;
            }

            BoundedWaiterState expected = BoundedWaiterState::kWaiting;
            if (!waiter->state.compare_exchange_strong(
                    expected,
                    BoundedWaiterState::kFulfilling,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                if (timeoutOperationStarted) {
                    const auto aborted = timeoutTimer->abortOperation();
                    if (aborted == TimeoutTimer::OperationAbort::kTimeoutWon) {
                        waiter->state.store(BoundedWaiterState::kCancelled,
                                            std::memory_order_release);
                        removeQueuedWaiter(m_recvWaiters, waiter);
                        pendingWakes.recv.setTimeoutWinner(waiter->timeoutTimer);
                        return BoundedWaiterProgress::kSkipped;
                    }
                    if (aborted == TimeoutTimer::OperationAbort::kCompleted) {
                        removeQueuedWaiter(m_recvWaiters, waiter);
                        return BoundedWaiterProgress::kSkipped;
                    }
                }
                return BoundedWaiterProgress::kNotClaimed;
            }
        }
    }

    /**
     * @brief 尝试推进并唤醒一个等待中的接收者。
     *
     * @param pendingWakes 收集需要在最后一次 channel 访问后执行的唤醒。
     *
     * @note 跳过已取消或被其他路径认领的 waiter；处理一个有效 waiter 后立即返回。
     */
    void wakeOneConsumerIfAny(PendingWakes& pendingWakes) noexcept
    {
        WaiterLease lease;
        while (tryDequeueWaiter(m_recvWaiters, lease)) {
            const auto progress =
                tryCompleteRecvWaiter(lease.get(), true, pendingWakes);
            if (progress == BoundedWaiterProgress::kCompleted ||
                progress == BoundedWaiterProgress::kWaiting) {
                return;
            }
        }
    }

    /**
     * @brief 在消费者释放容量后尝试推进一个等待中的发送者。
     *
     * @param pendingWakes 收集需要在最后一次 channel 访问后执行的唤醒。
     *
     * @note 跳过已取消或被其他路径认领的 waiter；处理一个有效 waiter 后立即返回。
     */
    void drainOneSendWaiter(PendingWakes& pendingWakes) noexcept
    {
        WaiterLease lease;
        while (tryDequeueWaiter(m_sendWaiters, lease)) {
            const auto progress =
                tryCompleteSendWaiter(lease.get(), true, pendingWakes);
            if (progress == BoundedWaiterProgress::kCompleted ||
                progress == BoundedWaiterProgress::kWaiting) {
                return;
            }
        }
    }

    /**
     * @brief 关闭时通过接收完成权裁决依次结束所有可认领接收 waiter。
     *
     * @param pendingWakes 收集需要在最后一次 channel 访问后执行的唤醒。
     */
    void wakeAllRecvWaiters(PendingWakes& pendingWakes) noexcept
    {
        WaiterLease lease;
        while (tryDequeueWaiter(m_recvWaiters, lease)) {
            const auto progress =
                tryCompleteRecvWaiter(lease.get(), true, pendingWakes);
            if (progress == BoundedWaiterProgress::kWaiting) {
                return;
            }
        }
    }

    /**
     * @brief 关闭时通过发送完成权裁决依次结束所有可认领发送 waiter。
     *
     * @param pendingWakes 收集需要在最后一次 channel 访问后执行的唤醒。
     */
    void wakeAllSendWaiters(PendingWakes& pendingWakes) noexcept
    {
        WaiterLease lease;
        while (tryDequeueWaiter(m_sendWaiters, lease)) {
            const auto progress =
                tryCompleteSendWaiter(lease.get(), true, pendingWakes);
            if (progress == BoundedWaiterProgress::kWaiting) {
                return;
            }
        }
    }

    /**
     * @brief 发布窗口结束后完成 close 延迟的两侧 waiter 收尾。
     *
     * @param pendingWakes 收集需要在最后一次 channel 访问后执行的唤醒。
     */
    void finishCloseAfterPublication(PendingWakes& pendingWakes) noexcept
    {
        wakeAllRecvWaiters(pendingWakes);
        wakeAllSendWaiters(pendingWakes);
    }

    template <BoundedValue U, size_t UCapacity>
    friend class BoundedSendAwaitable;
    template <BoundedValue U, size_t UCapacity>
    friend class BoundedRecvAwaitable;
    template <BoundedValue U, size_t UCapacity>
    friend class BoundedRecvBatchAwaitable;
    template <BoundedValue U, size_t UCapacity>
    friend class BoundedRecvBatchToAwaitable;

    // producer 只写 tail，consumer 只写 head。waiter helping 可能让同一逻辑侧
    // 迁移到另一物理线程，因此 cursor 保持 atomic，但数据面不执行 cursor CAS。
    alignas(::galay::utils::kCacheLineSize) std::atomic<size_t> m_tail{0};
    alignas(::galay::utils::kCacheLineSize) std::atomic<size_t> m_head{0};
    alignas(::galay::utils::kCacheLineSize) std::atomic<bool> m_closed{false};
    // Once an async waiter path has been used, keep the waiter-aware path enabled.
    // Before that point, synchronous trySend/tryRecv avoid polling empty waiter queues.
    alignas(::galay::utils::kCacheLineSize)
        std::atomic<bool> m_recvWaiterPathUsed{false};
    alignas(::galay::utils::kCacheLineSize)
        std::atomic<bool> m_sendWaiterPathUsed{false};
    alignas(::galay::utils::kCacheLineSize) SlotStorage m_slots;
    size_t m_capacity = 0;
    size_t m_mask = 0;
    bool m_useAsymmetricBarrier =
        kernel::detail::asymmetricMemoryBarrierSupport().has_value();
    RingError m_error = RingError::kNone;
    alignas(::galay::utils::kCacheLineSize) Waiter m_recvWaiterSlot;
    alignas(::galay::utils::kCacheLineSize) Waiter m_sendWaiterSlot;
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

template <BoundedValue T, size_t Capacity>
    requires detail::ValidBoundedChannelCapacity<Capacity>
inline BoundedSendAwaitable<T, Capacity>
BoundedChannel<T, Capacity>::send(T&& value)
{
    return BoundedSendAwaitable<T, Capacity>(this, std::move(value));
}

template <BoundedValue T, size_t Capacity>
    requires detail::ValidBoundedChannelCapacity<Capacity>
inline BoundedRecvAwaitable<T, Capacity> BoundedChannel<T, Capacity>::recv()
{
    return BoundedRecvAwaitable<T, Capacity>(this);
}

template <BoundedValue T, size_t Capacity>
    requires detail::ValidBoundedChannelCapacity<Capacity>
inline BoundedRecvBatchAwaitable<T, Capacity>
BoundedChannel<T, Capacity>::recvBatch(size_t count)
{
    return BoundedRecvBatchAwaitable<T, Capacity>(this, count);
}

template <BoundedValue T, size_t Capacity>
    requires detail::ValidBoundedChannelCapacity<Capacity>
inline BoundedRecvBatchToAwaitable<T, Capacity>
BoundedChannel<T, Capacity>::recvBatchTo(
    std::span<T> output) noexcept
    requires std::is_nothrow_move_assignable_v<T>
{
    return BoundedRecvBatchToAwaitable<T, Capacity>(this, output);
}

template <BoundedValue T, size_t Capacity>
inline bool BoundedSendAwaitable<T, Capacity>::await_ready() noexcept
{
    if (m_channel->trySend(std::move(m_value))) {
        m_sent = true;
        return true;
    }
    return !m_channel->isValid() || m_channel->isClosed();
}

template <BoundedValue T, size_t Capacity>
template <typename Promise>
inline bool BoundedSendAwaitable<T, Capacity>::await_suspend(
    std::coroutine_handle<Promise> handle) noexcept
{
    auto* channel = m_channel;
    if (channel->trySend(std::move(m_value))) {
        m_sent = true;
        return false;
    }
    if (!channel->isValid() || channel->isClosed()) {
        return false;
    }

    TimeoutTimer::ptr timeoutTimer = std::move(m_timeoutTimer);
    auto* const waiter = &channel->m_sendWaiterSlot;
    if (!waiter->begin(Waker(handle),
                       std::move(timeoutTimer),
                       &m_value,
                       nullptr,
                       m_waiterGeneration)) {
        m_registrationFailed = true;
        return false;
    }
    m_waiter = waiter;
    const bool waiterPathWasUsed =
        channel->m_sendWaiterPathUsed.exchange(
            true, std::memory_order_seq_cst);
    if (auto barrierError =
            channel->firstWaiterBarrierError(!waiterPathWasUsed);
        barrierError.has_value()) {
        m_registrationSystemError = *barrierError;
        channel->m_sendWaiterPathUsed.store(false,
                                            std::memory_order_seq_cst);
        waiter->state.store(BoundedWaiterState::kFailed,
                            std::memory_order_release);
        return false;
    }
    if (!channel->enqueueWaiter(channel->m_sendWaiters, waiter)) {
        waiter->state.store(BoundedWaiterState::kFailed, std::memory_order_release);
        return false;
    }
    channel->synchronizeSendWaiterPath();

    typename BoundedChannel<T, Capacity>::PendingWakes pendingWakes;
    const auto progress = channel->tryCompleteSendWaiter(waiter, false, pendingWakes);
    const bool suspend =
        progress != BoundedWaiterProgress::kCompleted &&
        progress != BoundedWaiterProgress::kSkipped && waiter->finishArming();
    pendingWakes.wakeAll();
    return suspend;
}

template <BoundedValue T, size_t Capacity>
inline std::expected<void, IOError>
BoundedSendAwaitable<T, Capacity>::await_resume() noexcept
{
    if (m_sent) {
        return {};
    }
    if (m_registrationFailed) {
        return std::unexpected(IOError(kNotReady, 0));
    }
    if (m_waiter != nullptr) {
        detail::waitForBoundedChannelFulfillment(*m_waiter);
        const auto state = m_waiter->state.load(std::memory_order_acquire);
        const bool reclaimed = m_channel->reclaimWaiter(
            m_channel->m_sendWaiters, *m_waiter, m_waiterGeneration);
        m_waiter = nullptr;
        m_waiterGeneration = 0;
        if (!reclaimed) {
            return std::unexpected(IOError(kNotReady, 0));
        }
        if (state == BoundedWaiterState::kFulfilled) {
            return {};
        }
        if (state == BoundedWaiterState::kClosed) {
            return std::unexpected(IOError(kClosed, 0));
        }
        if (state == BoundedWaiterState::kFailed) {
            return std::unexpected(
                IOError(kNotReady, m_registrationSystemError));
        }
    }
    if (m_timedOut) {
        return std::unexpected(IOError(kTimeout, 0));
    }
    if (!m_channel->isValid()) {
        return std::unexpected(m_channel->constructionError());
    }
    if (m_channel->isClosed()) {
        return std::unexpected(IOError(kClosed, 0));
    }
    return std::unexpected(IOError(kNotReady, 0));
}

template <BoundedValue T, size_t Capacity>
inline void BoundedSendAwaitable<T, Capacity>::markTimeout() noexcept
{
    m_timedOut = true;
    if (m_waiter == nullptr ||
        m_waiter->generation.load(std::memory_order_acquire) !=
            m_waiterGeneration) {
        return;
    }
    BoundedWaiterState expected = BoundedWaiterState::kWaiting;
    if (m_waiter->state.compare_exchange_strong(expected,
                                                BoundedWaiterState::kCancelled,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
        m_channel->removeQueuedWaiter(m_channel->m_sendWaiters, m_waiter);
    } else if (expected == BoundedWaiterState::kCancelled) {
        m_channel->removeQueuedWaiter(m_channel->m_sendWaiters, m_waiter);
    }
}

template <BoundedValue T, size_t Capacity>
inline bool BoundedRecvAwaitable<T, Capacity>::await_ready() noexcept
{
    if (auto value = m_channel->tryRecv(); value.has_value()) {
        m_ready.emplace(std::move(*value));
        return true;
    }
    return !m_channel->isValid() || m_channel->isReceiveClosed();
}

template <BoundedValue T, size_t Capacity>
template <typename Promise>
inline bool BoundedRecvAwaitable<T, Capacity>::await_suspend(
    std::coroutine_handle<Promise> handle) noexcept
{
    auto* channel = m_channel;
    if (auto value = channel->tryRecv(); value.has_value()) {
        m_ready.emplace(std::move(*value));
        return false;
    }
    if (!channel->isValid() || channel->isReceiveClosed()) {
        return false;
    }

    TimeoutTimer::ptr timeoutTimer = std::move(m_timeoutTimer);
    auto* const waiter = &channel->m_recvWaiterSlot;
    if (!waiter->begin(Waker(handle),
                       std::move(timeoutTimer),
                       nullptr,
                       &m_ready,
                       m_waiterGeneration)) {
        m_registrationFailed = true;
        return false;
    }
    m_waiter = waiter;
    const bool waiterPathWasUsed =
        channel->m_recvWaiterPathUsed.exchange(
            true, std::memory_order_seq_cst);
    if (auto barrierError =
            channel->firstWaiterBarrierError(!waiterPathWasUsed);
        barrierError.has_value()) {
        m_registrationSystemError = *barrierError;
        channel->m_recvWaiterPathUsed.store(false,
                                            std::memory_order_seq_cst);
        waiter->state.store(BoundedWaiterState::kFailed,
                            std::memory_order_release);
        return false;
    }
    if (!channel->enqueueWaiter(channel->m_recvWaiters, waiter)) {
        waiter->state.store(BoundedWaiterState::kFailed, std::memory_order_release);
        return false;
    }
    channel->synchronizeRecvWaiterPath();

    typename BoundedChannel<T, Capacity>::PendingWakes pendingWakes;
    const auto progress = channel->tryCompleteRecvWaiter(waiter, false, pendingWakes);
    const bool suspend =
        progress != BoundedWaiterProgress::kCompleted &&
        progress != BoundedWaiterProgress::kSkipped && waiter->finishArming();
    pendingWakes.wakeAll();
    return suspend;
}

template <BoundedValue T, size_t Capacity>
inline std::expected<T, IOError>
BoundedRecvAwaitable<T, Capacity>::await_resume() noexcept
{
    if (m_registrationFailed) {
        return std::unexpected(IOError(kNotReady, 0));
    }
    if (m_waiter != nullptr) {
        detail::waitForBoundedChannelFulfillment(*m_waiter);
        const auto state = m_waiter->state.load(std::memory_order_acquire);
        const bool reclaimed = m_channel->reclaimWaiter(
            m_channel->m_recvWaiters, *m_waiter, m_waiterGeneration);
        m_waiter = nullptr;
        m_waiterGeneration = 0;
        if (!reclaimed) {
            return std::unexpected(IOError(kNotReady, 0));
        }
        if (state == BoundedWaiterState::kFulfilled && m_ready.has_value()) {
            return std::move(*m_ready);
        }
        if (state == BoundedWaiterState::kClosed) {
            return std::unexpected(IOError(kClosed, 0));
        }
        if (state == BoundedWaiterState::kFailed) {
            return std::unexpected(
                IOError(kNotReady, m_registrationSystemError));
        }
        if (state == BoundedWaiterState::kFulfilled) {
            return std::unexpected(IOError(kClosed, 0));
        }
    }
    if (m_ready.has_value()) {
        return std::move(*m_ready);
    }
    if (m_timedOut) {
        return std::unexpected(IOError(kTimeout, 0));
    }
    if (!m_channel->isValid()) {
        return std::unexpected(m_channel->constructionError());
    }
    if (m_channel->isReceiveClosed()) {
        return std::unexpected(IOError(kClosed, 0));
    }
    if (auto value = m_channel->tryRecv(); value.has_value()) {
        return std::move(*value);
    }
    return std::unexpected(IOError(kNotReady, 0));
}

template <BoundedValue T, size_t Capacity>
inline void BoundedRecvAwaitable<T, Capacity>::markTimeout() noexcept
{
    m_timedOut = true;
    if (m_waiter == nullptr ||
        m_waiter->generation.load(std::memory_order_acquire) !=
            m_waiterGeneration) {
        return;
    }
    BoundedWaiterState expected = BoundedWaiterState::kWaiting;
    if (m_waiter->state.compare_exchange_strong(expected,
                                                BoundedWaiterState::kCancelled,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
        m_channel->removeQueuedWaiter(m_channel->m_recvWaiters, m_waiter);
    } else if (expected == BoundedWaiterState::kCancelled) {
        m_channel->removeQueuedWaiter(m_channel->m_recvWaiters, m_waiter);
    }
}

template <BoundedValue T, size_t Capacity>
inline bool BoundedRecvBatchToAwaitable<T, Capacity>::tryReceiveNow() noexcept
{
    if (m_ready) {
        return true;
    }
    if (m_output.empty()) {
        m_ready = true;
        return true;
    }
    m_readyCount = m_channel->tryRecvBatch(m_output);
    m_ready = m_readyCount != 0;
    return m_ready;
}

template <BoundedValue T, size_t Capacity>
inline bool BoundedRecvBatchToAwaitable<T, Capacity>::await_ready() noexcept
{
    return tryReceiveNow() || !m_channel->isValid() ||
        m_channel->isReceiveClosed();
}

template <BoundedValue T, size_t Capacity>
template <typename Promise>
inline bool BoundedRecvBatchToAwaitable<T, Capacity>::await_suspend(
    std::coroutine_handle<Promise> handle) noexcept
{
    auto* channel = m_channel;
    if (tryReceiveNow()) {
        return false;
    }
    if (!channel->isValid() || channel->isReceiveClosed()) {
        return false;
    }

    TimeoutTimer::ptr timeoutTimer = std::move(m_timeoutTimer);
    auto* const waiter = &channel->m_recvWaiterSlot;
    if (!waiter->begin(Waker(handle),
                       std::move(timeoutTimer),
                       nullptr,
                       &m_waiterReady,
                       m_waiterGeneration)) {
        m_registrationFailed = true;
        return false;
    }
    m_waiter = waiter;
    const bool waiterPathWasUsed =
        channel->m_recvWaiterPathUsed.exchange(
            true, std::memory_order_seq_cst);
    if (auto barrierError =
            channel->firstWaiterBarrierError(!waiterPathWasUsed);
        barrierError.has_value()) {
        m_registrationSystemError = *barrierError;
        channel->m_recvWaiterPathUsed.store(false,
                                            std::memory_order_seq_cst);
        waiter->state.store(BoundedWaiterState::kFailed,
                            std::memory_order_release);
        return false;
    }
    if (!channel->enqueueWaiter(channel->m_recvWaiters, waiter)) {
        waiter->state.store(BoundedWaiterState::kFailed,
                            std::memory_order_release);
        return false;
    }
    channel->synchronizeRecvWaiterPath();

    typename BoundedChannel<T, Capacity>::PendingWakes pendingWakes;
    const auto progress =
        channel->tryCompleteRecvWaiter(waiter, false, pendingWakes);
    const bool suspend =
        progress != BoundedWaiterProgress::kCompleted &&
        progress != BoundedWaiterProgress::kSkipped && waiter->finishArming();
    pendingWakes.wakeAll();
    return suspend;
}

template <BoundedValue T, size_t Capacity>
inline std::expected<size_t, IOError>
BoundedRecvBatchToAwaitable<T, Capacity>::await_resume() noexcept
{
    if (m_ready) {
        return m_readyCount;
    }
    if (m_registrationFailed) {
        return std::unexpected(IOError(kNotReady, 0));
    }
    if (m_waiter != nullptr) {
        detail::waitForBoundedChannelFulfillment(*m_waiter);
        const auto state = m_waiter->state.load(std::memory_order_acquire);
        const bool reclaimed = m_channel->reclaimWaiter(
            m_channel->m_recvWaiters, *m_waiter, m_waiterGeneration);
        m_waiter = nullptr;
        m_waiterGeneration = 0;
        if (!reclaimed) {
            return std::unexpected(IOError(kNotReady, 0));
        }
        if (state == BoundedWaiterState::kFulfilled &&
            m_waiterReady.has_value() && !m_output.empty()) {
            m_output[0] = std::move(*m_waiterReady);
            m_waiterReady.reset();
            m_readyCount = 1;
            if (m_output.size() > 1) {
                m_readyCount +=
                    m_channel->tryRecvBatch(m_output.subspan(1));
            }
            m_ready = true;
            return m_readyCount;
        }
        if (state == BoundedWaiterState::kClosed ||
            state == BoundedWaiterState::kFulfilled) {
            return std::unexpected(IOError(kClosed, 0));
        }
        if (state == BoundedWaiterState::kFailed) {
            return std::unexpected(
                IOError(kNotReady, m_registrationSystemError));
        }
    }
    if (m_timedOut) {
        return std::unexpected(IOError(kTimeout, 0));
    }
    if (!m_channel->isValid()) {
        return std::unexpected(m_channel->constructionError());
    }
    if (m_channel->isReceiveClosed()) {
        return std::unexpected(IOError(kClosed, 0));
    }
    if (tryReceiveNow()) {
        return m_readyCount;
    }
    return std::unexpected(IOError(kNotReady, 0));
}

template <BoundedValue T, size_t Capacity>
inline void BoundedRecvBatchToAwaitable<T, Capacity>::markTimeout() noexcept
{
    m_timedOut = true;
    if (m_waiter == nullptr ||
        m_waiter->generation.load(std::memory_order_acquire) !=
            m_waiterGeneration) {
        return;
    }
    BoundedWaiterState expected = BoundedWaiterState::kWaiting;
    if (m_waiter->state.compare_exchange_strong(expected,
                                                BoundedWaiterState::kCancelled,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
        m_channel->removeQueuedWaiter(m_channel->m_recvWaiters, m_waiter);
    } else if (expected == BoundedWaiterState::kCancelled) {
        m_channel->removeQueuedWaiter(m_channel->m_recvWaiters, m_waiter);
    }
}

template <BoundedValue T, size_t Capacity>
inline bool BoundedRecvBatchAwaitable<T, Capacity>::tryReceiveNow()
{
    if (m_batchReady) {
        return true;
    }
    if (m_count == 0) {
        m_batchReady = true;
        return true;
    }
    if (!m_channel->hasReadySlot()) {
        return false;
    }
    // 先完成 owning vector 分配，再消费 ring，保证 allocator 失败时队列不变。
    m_values.reserve(m_count);
    auto first = m_channel->tryRecv();
    if (!first.has_value()) {
        return false;
    }
    m_values.push_back(std::move(*first));
    while (m_values.size() < m_count) {
        auto value = m_channel->tryRecv();
        if (!value.has_value()) {
            break;
        }
        m_values.push_back(std::move(*value));
    }
    m_batchReady = true;
    return true;
}

template <BoundedValue T, size_t Capacity>
inline bool BoundedRecvBatchAwaitable<T, Capacity>::await_ready()
{
    return tryReceiveNow() || !m_channel->isValid() ||
        m_channel->isReceiveClosed();
}

template <BoundedValue T, size_t Capacity>
template <typename Promise>
inline bool BoundedRecvBatchAwaitable<T, Capacity>::await_suspend(
    std::coroutine_handle<Promise> handle)
{
    auto* channel = m_channel;
    if (tryReceiveNow()) {
        return false;
    }
    if (!channel->isValid() || channel->isReceiveClosed()) {
        return false;
    }

    TimeoutTimer::ptr timeoutTimer = std::move(m_timeoutTimer);
    auto* const waiter = &channel->m_recvWaiterSlot;
    if (!waiter->begin(Waker(handle),
                       std::move(timeoutTimer),
                       nullptr,
                       nullptr,
                       m_waiterGeneration)) {
        m_registrationFailed = true;
        return false;
    }
    m_waiter = waiter;
    const bool waiterPathWasUsed =
        channel->m_recvWaiterPathUsed.exchange(
            true, std::memory_order_seq_cst);
    if (auto barrierError =
            channel->firstWaiterBarrierError(!waiterPathWasUsed);
        barrierError.has_value()) {
        m_registrationSystemError = *barrierError;
        channel->m_recvWaiterPathUsed.store(false,
                                            std::memory_order_seq_cst);
        waiter->state.store(BoundedWaiterState::kFailed,
                            std::memory_order_release);
        return false;
    }
    if (!channel->enqueueWaiter(channel->m_recvWaiters, waiter)) {
        waiter->state.store(BoundedWaiterState::kFailed, std::memory_order_release);
        return false;
    }
    channel->synchronizeRecvWaiterPath();

    typename BoundedChannel<T, Capacity>::PendingWakes pendingWakes;
    const auto progress = channel->tryCompleteRecvWaiter(waiter, false, pendingWakes);
    const bool suspend =
        progress != BoundedWaiterProgress::kCompleted &&
        progress != BoundedWaiterProgress::kSkipped && waiter->finishArming();
    pendingWakes.wakeAll();
    return suspend;
}

template <BoundedValue T, size_t Capacity>
inline std::expected<std::vector<T>, IOError>
BoundedRecvBatchAwaitable<T, Capacity>::await_resume()
{
    if (m_batchReady) {
        return std::move(m_values);
    }
    if (m_registrationFailed) {
        return std::unexpected(IOError(kNotReady, 0));
    }
    if (m_waiter != nullptr) {
        detail::waitForBoundedChannelFulfillment(*m_waiter);
        const auto state = m_waiter->state.load(std::memory_order_acquire);
        const bool reclaimed = m_channel->reclaimWaiter(
            m_channel->m_recvWaiters, *m_waiter, m_waiterGeneration);
        m_waiter = nullptr;
        m_waiterGeneration = 0;
        if (!reclaimed) {
            return std::unexpected(IOError(kNotReady, 0));
        }
        if (state == BoundedWaiterState::kFulfilled && tryReceiveNow()) {
            return std::move(m_values);
        }
        if (state == BoundedWaiterState::kClosed || state == BoundedWaiterState::kFulfilled) {
            return std::unexpected(IOError(kClosed, 0));
        }
        if (state == BoundedWaiterState::kFailed) {
            return std::unexpected(
                IOError(kNotReady, m_registrationSystemError));
        }
    }
    if (m_timedOut) {
        return std::unexpected(IOError(kTimeout, 0));
    }
    if (!m_channel->isValid()) {
        return std::unexpected(m_channel->constructionError());
    }
    if (m_channel->isReceiveClosed()) {
        return std::unexpected(IOError(kClosed, 0));
    }
    if (tryReceiveNow()) {
        return std::move(m_values);
    }
    return std::unexpected(IOError(kNotReady, 0));
}

template <BoundedValue T, size_t Capacity>
inline void BoundedRecvBatchAwaitable<T, Capacity>::markTimeout() noexcept
{
    m_timedOut = true;
    if (m_waiter == nullptr ||
        m_waiter->generation.load(std::memory_order_acquire) !=
            m_waiterGeneration) {
        return;
    }
    BoundedWaiterState expected = BoundedWaiterState::kWaiting;
    if (m_waiter->state.compare_exchange_strong(expected,
                                                BoundedWaiterState::kCancelled,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
        m_channel->removeQueuedWaiter(m_channel->m_recvWaiters, m_waiter);
    } else if (expected == BoundedWaiterState::kCancelled) {
        m_channel->removeQueuedWaiter(m_channel->m_recvWaiters, m_waiter);
    }
}


} // namespace galay::spsc

#endif // GALAY_CONCURRENCY_SPSC_BOUNDED_CHANNEL_H
