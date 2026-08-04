/**
 * @file unbounded_channel.h
 * @brief 无界 MPMC 异步通道。
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 使用可回收分段保存消息，并使用无锁等待队列管理挂起的多个
 * 消费者。发送不会因容量不足挂起；空队列接收只挂起协程，不阻塞调度器线程。
 */

#ifndef GALAY_CONCURRENCY_MPMC_UNBOUNDED_CHANNEL_H
#define GALAY_CONCURRENCY_MPMC_UNBOUNDED_CHANNEL_H

#include "../../common/error.h"
#include "../../core/timeout.hpp"
#include "../../core/waker.h"
#include "../../../galay-utils/common/defn.hpp"

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <emmintrin.h>
#endif

#include <atomic>
#include <array>
#include <concepts>
#include <concurrentqueue/moodycamel/concurrentqueue.h>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <memory>
#include <new>
#include <optional>
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
 * @brief 约束 MPMC 无界通道可存储的元素类型。
 * @tparam T 元素类型；底层无界队列要求可移动且可默认构造，所有数据搬运操作
 *           必须不抛异常，避免 producer active publication 无法收尾。
 */
template <typename T>
concept UnboundedValue = std::movable<T> && std::default_initializable<T> &&
    std::is_nothrow_default_constructible_v<T> &&
    std::is_nothrow_move_constructible_v<T> &&
    std::is_nothrow_move_assignable_v<T>;

enum class UnboundedWaiterState : uint8_t {
    kWaiting,
    kCancelled,
    kFulfilling,
    kFulfilled,
    kClosed,
    kFailed
};

enum class UnboundedWaiterProgress : uint8_t {
    kBlocked,    ///< waiter 仍存活但当前没有消息可交付。
    kProgressed, ///< waiter 已完成并消费一次消息或关闭事件。
    kSkipped     ///< stale/timeout waiter 未消费当前消息事件。
};

template <UnboundedValue T>
struct UnboundedChannelWaiter
{
    std::optional<T> value;
    kernel::detail::DeferredWaker localWaker;
    const TimeoutTimer::ptr timeoutTimer; ///< 发布后只读，避免 stale entry 与完成方并发修改。
    kernel::detail::DeferredWaker* const completionWaker;
    std::atomic<UnboundedWaiterState> state{UnboundedWaiterState::kWaiting};
    std::atomic<bool> queued{false};

    explicit UnboundedChannelWaiter(Waker waiter_waker,
                                    TimeoutTimer::ptr timeout_timer = {})
        : localWaker(timeout_timer ? Waker() : std::move(waiter_waker))
        , timeoutTimer(std::move(timeout_timer))
        , completionWaker(timeoutTimer != nullptr
              ? &timeoutTimer->completionWaker()
              : &localWaker)
    {
    }
};

namespace detail {

inline void unboundedChannelCpuPause() noexcept
{
#if defined(_MSC_VER)
    YieldProcessor();
#elif defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__APPLE__) && defined(__aarch64__)
    __asm__ __volatile__("isb" ::: "memory");
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#endif
}

inline void unboundedChannelBackoff(uint32_t& step) noexcept
{
    const uint32_t count = uint32_t{1} << (step < 6 ? step : 6);
    for (uint32_t index = 0; index < count; ++index) {
        unboundedChannelCpuPause();
    }
    if (step < 6) {
        ++step;
    }
}

template <UnboundedValue T>
void waitForUnboundedChannelFulfillment(UnboundedChannelWaiter<T>& waiter) noexcept
{
    while (waiter.state.load(std::memory_order_acquire) ==
           UnboundedWaiterState::kFulfilling) {
        unboundedChannelCpuPause();
    }
}

} // namespace detail

template <UnboundedValue T>
class UnboundedChannel;

struct UnboundedChannelTestAccess;

template <UnboundedValue T>
class UnboundedRecvAwaitable;

template <UnboundedValue T>
class UnboundedRecvBatchAwaitable;

/**
 * @brief MPMC 无界通道的异步单条接收等待体。
 * @tparam T 通道元素类型。
 * @details 空队列时挂起协程；关闭和超时通过 std::expected 的 IOError 返回。
 */
template <UnboundedValue T>
class UnboundedRecvAwaitable : public TimeoutSupport<UnboundedRecvAwaitable<T>>
{
public:
    /** @brief 创建绑定指定通道的异步接收等待体。 */
    explicit UnboundedRecvAwaitable(UnboundedChannel<T>* channel)
        : m_channel(channel)
    {
    }

    /** @brief 已有消息或通道已关闭时返回 true。 */
    bool await_ready() noexcept;

    /**
     * @brief 注册当前协程为接收等待者。
     * @return 需要等待消息时返回 true；已同步完成或注册失败时返回 false。
     * @note 该函数只挂起协程，不阻塞调度器线程。
     */
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) noexcept;

    /** @brief 返回消息；关闭、超时或等待队列失败通过 IOError 返回。 */
    std::expected<T, IOError> await_resume() noexcept;

    /** @brief 尝试取消尚未被发送或关闭路径认领的等待者。 */
    void markTimeout() noexcept;

private:
    friend struct WithTimeout<UnboundedRecvAwaitable<T>>;

    void bindTimeoutTimer(const TimeoutTimer::ptr& timer) noexcept
    {
        m_timeoutTimer = timer;
    }

    UnboundedChannel<T>* m_channel;
    TimeoutTimer::ptr m_timeoutTimer;
    std::optional<T> m_ready;
    std::shared_ptr<UnboundedChannelWaiter<T>> m_waiter;
    bool m_timedOut = false;
};

/**
 * @brief MPMC 无界通道的异步批量接收等待体。
 * @tparam T 通道元素类型。
 * @details 至少等待一条消息，恢复后尽量补齐到 count；不会阻塞调度器线程。
 */
template <UnboundedValue T>
class UnboundedRecvBatchAwaitable : public TimeoutSupport<UnboundedRecvBatchAwaitable<T>>
{
public:
    /**
     * @brief 创建异步批量接收等待体。
     * @param channel 目标通道，等待完成前必须保持有效。
     * @param count 单次最多接收的消息数；0 表示立即返回空批次。
     */
    UnboundedRecvBatchAwaitable(UnboundedChannel<T>* channel, size_t count)
        : m_channel(channel), m_count(count)
    {
    }

    /** @brief 已取得批次或通道已关闭时返回 true。 */
    bool await_ready() noexcept;

    /**
     * @brief 注册当前协程为接收等待者。
     * @return 需要等待消息时返回 true；已同步完成或注册失败时返回 false。
     * @note 该函数只挂起协程，不阻塞调度器线程。
     */
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) noexcept;

    /** @brief 返回消息批次；关闭、超时或等待队列失败通过 IOError 返回。 */
    std::expected<std::vector<T>, IOError> await_resume() noexcept;

    /** @brief 尝试取消尚未被发送或关闭路径认领的等待者。 */
    void markTimeout() noexcept;

private:
    friend struct WithTimeout<UnboundedRecvBatchAwaitable<T>>;

    void bindTimeoutTimer(const TimeoutTimer::ptr& timer) noexcept
    {
        m_timeoutTimer = timer;
    }

    UnboundedChannel<T>* m_channel;
    TimeoutTimer::ptr m_timeoutTimer;
    size_t m_count;
    std::optional<std::vector<T>> m_ready;
    std::shared_ptr<UnboundedChannelWaiter<T>> m_waiter;
    bool m_timedOut = false;
};

/**
 * @brief 线程安全的无界 MPMC 异步通道。
 * @tparam T 元素类型，要求默认构造、移动构造和移动赋值均不抛异常。
 * @details 多个生产者和多个消费者可以并发访问。发送只会在底层队列内存申请失败时
 *          返回 false；接收为空时可通过 recv()/recvBatch() 挂起协程。
 * @note 通道具有唯一身份，不可复制或移动；必须存活到所有挂起操作完成或超时。
 */
template <UnboundedValue T>
class UnboundedChannel
{
private:
    // AArch64 平台按 128B 隔离 producer publication 与 waiter 热状态；
    // 其他当前支持平台使用 64B。固定值由 galay-utils/common/defn.hpp 统一提供。
    static constexpr uint8_t kPumpRunning = 1U;
    static constexpr uint8_t kRecvWork = 1U << 1U;
    static constexpr uint8_t kRecvWaiterPathUsed = 1U;
    static constexpr uint8_t kRecvCleanupPending = 1U << 1U;
    static constexpr uint64_t kSlotsPerBlock = 4096;
    static constexpr uint64_t kClosedBit = uint64_t{1} << 63U;
    static constexpr uint64_t kPositionMask = kClosedBit - 1;
    static constexpr uint64_t kInvalidBlockBase = UINT64_MAX;

    enum class BlockState : uint8_t {
        kActive,
        kRetired,
    };

    struct Slot
    {
        T* value() noexcept
        {
            return std::launder(reinterpret_cast<T*>(storage));
        }

        std::atomic<uint64_t> sequence{0};
        alignas(T) std::byte storage[sizeof(T)];
    };

    struct alignas(::galay::utils::kCacheLineSize) Block
    {
        std::atomic<Block*> next{nullptr};
        std::atomic<uint64_t> base{kInvalidBlockBase};
        std::atomic<BlockState> state{BlockState::kActive};
        Block* poolNext = nullptr;
        Block* allNext = nullptr;
        std::array<Slot, kSlotsPerBlock> slots;
    };

    struct DefaultProducerCacheEntry
    {
        const UnboundedChannel* channel = nullptr;
        Block* block = nullptr;
        DefaultProducerCacheEntry* next = nullptr;
        uint64_t generation = 0;
        uint64_t blockBase = kInvalidBlockBase;
    };

    /**
     * @brief 默认 send() 的线程本地 producer 缓存。
     * @note 析构只释放缓存节点，绝不访问已可能销毁的 channel 或 producer。
     */
    struct DefaultProducerCache
    {
        DefaultProducerCache() = default;
        ~DefaultProducerCache() noexcept
        {
            while (head != nullptr) {
                DefaultProducerCacheEntry* next = head->next;
                delete head;
                head = next;
            }
        }
        DefaultProducerCache(const DefaultProducerCache&) = delete;
        DefaultProducerCache& operator=(const DefaultProducerCache&) = delete;
        DefaultProducerCache(DefaultProducerCache&&) = delete;
        DefaultProducerCache& operator=(DefaultProducerCache&&) = delete;

        DefaultProducerCacheEntry* head = nullptr;
    };

public:
    /**
     * @brief 绑定当前通道和单个生产线程的显式生产者 token。
     * @details token 复用 moodycamel 的显式 producer，避免每次发送查找隐式 producer。
     *          同一 token 不得被多个线程并发使用，且通道必须比 token 存活更久。
     */
    class ProducerToken
    {
    public:
        ProducerToken(const ProducerToken&) = delete;
        ProducerToken& operator=(const ProducerToken&) = delete;

        ProducerToken(ProducerToken&& other) noexcept
            : m_block(std::exchange(other.m_block, nullptr)),
              m_channel(std::exchange(other.m_channel, nullptr)),
              m_generation(std::exchange(other.m_generation, 0)),
              m_blockBase(std::exchange(other.m_blockBase, kInvalidBlockBase))
        {
        }

        ProducerToken& operator=(ProducerToken&& other) noexcept
        {
            if (this != &other) {
                std::swap(m_block, other.m_block);
                std::swap(m_channel, other.m_channel);
                std::swap(m_generation, other.m_generation);
                std::swap(m_blockBase, other.m_blockBase);
            }
            return *this;
        }

        /** @brief 返回 token 是否仍绑定有效通道 producer。 */
        bool valid() const noexcept
        {
            return m_channel != nullptr && m_generation != 0;
        }

    private:
        friend class UnboundedChannel;
        friend struct UnboundedChannelTestAccess;

        explicit ProducerToken(UnboundedChannel& channel)
            : m_block(channel.m_tailBlock.load(std::memory_order_acquire)),
              m_channel(&channel),
              m_generation(channel.m_generation),
              m_blockBase(m_block != nullptr
                    ? m_block->base.load(std::memory_order_acquire)
                    : kInvalidBlockBase)
        {
        }

        bool validFor(const UnboundedChannel& channel) const noexcept
        {
            return m_channel == &channel && m_generation == channel.m_generation;
        }

        Block* m_block;
        UnboundedChannel* m_channel;
        uint64_t m_generation;
        uint64_t m_blockBase;
    };

    /**
     * @brief 绑定当前通道和单个消费线程的显式消费者 token。
     * @details token 缓存 producer 扫描位置；同一 token 不得被多个线程并发使用，
     *          且通道必须比 token 存活更久。异步 recv 不接受 token，因为协程可能换线程恢复。
     */
    class ConsumerToken
    {
    public:
        ConsumerToken(const ConsumerToken&) = delete;
        ConsumerToken& operator=(const ConsumerToken&) = delete;

        ConsumerToken(ConsumerToken&& other) noexcept
            : m_block(std::exchange(other.m_block, nullptr)),
              m_channel(std::exchange(other.m_channel, nullptr)),
              m_blockBase(std::exchange(other.m_blockBase, kInvalidBlockBase))
        {
        }

        ConsumerToken& operator=(ConsumerToken&& other) noexcept
        {
            if (this != &other) {
                std::swap(m_block, other.m_block);
                std::swap(m_channel, other.m_channel);
                std::swap(m_blockBase, other.m_blockBase);
            }
            return *this;
        }

        /** @brief 返回 token 是否仍绑定有效通道。 */
        bool valid() const noexcept
        {
            return m_channel != nullptr;
        }

    private:
        friend class UnboundedChannel;
        friend struct UnboundedChannelTestAccess;

        explicit ConsumerToken(UnboundedChannel& channel)
            : m_block(channel.m_headBlock.load(std::memory_order_acquire)),
              m_channel(&channel),
              m_blockBase(m_block != nullptr
                    ? m_block->base.load(std::memory_order_acquire)
                    : kInvalidBlockBase)
        {
        }

        bool validFor(const UnboundedChannel& channel) const noexcept
        {
            return m_channel == &channel;
        }

        Block* m_block;
        UnboundedChannel* m_channel;
        uint64_t m_blockBase;
    };

    UnboundedChannel() noexcept
        : m_generation(nextGeneration())
    {
        Block* initial = allocateBlock(0);
        m_headBlock.store(initial, std::memory_order_relaxed);
        m_tailBlock.store(initial, std::memory_order_relaxed);
    }
    ~UnboundedChannel() noexcept
    {
        Block* block = m_allBlocks;
        while (block != nullptr) {
            Block* next = block->allNext;
            const uint64_t base = block->base.load(std::memory_order_relaxed);
            if (base != kInvalidBlockBase) {
                for (uint64_t offset = 0; offset < kSlotsPerBlock; ++offset) {
                    Slot& slot = block->slots[static_cast<size_t>(offset)];
                    if (slot.sequence.load(std::memory_order_relaxed) ==
                        base + offset + 1) {
                        std::destroy_at(slot.value());
                    }
                }
            }
            delete block;
            block = next;
        }
    }
    UnboundedChannel(const UnboundedChannel&) = delete;
    UnboundedChannel& operator=(const UnboundedChannel&) = delete;
    UnboundedChannel(UnboundedChannel&&) = delete;
    UnboundedChannel& operator=(UnboundedChannel&&) = delete;

    /** @brief 为当前生产线程创建显式 producer token。 */
    [[nodiscard]] ProducerToken makeProducerToken()
    {
        return ProducerToken(*this);
    }

    /** @brief 为当前消费线程创建显式 consumer token。 */
    [[nodiscard]] ConsumerToken makeConsumerToken()
    {
        return ConsumerToken(*this);
    }

    /**
     * @brief 发送一条消息。
     * @param value 待发送消息；成功交接或入队时移动。
     * @return true 表示发送成功；false 表示通道已关闭或底层队列拒绝入队。
     */
    bool send(T&& value)
    {
        return sendImpl(nullptr, std::move(value));
    }

    /**
     * @brief 使用线程独占 producer token 发送一条消息。
     * @return true 表示发送成功；token 不属于当前通道、通道关闭或入队失败时返回 false。
     */
    bool send(ProducerToken& token, T&& value)
    {
        if (!token.validFor(*this)) {
            return false;
        }
        return sendTokenFast<true>(token, std::move(value));
    }

    /** @brief 复制并发送一条消息。 */
    bool send(const T& value)
        requires std::copy_constructible<T> &&
            std::is_nothrow_copy_constructible_v<T>
    {
        T copy = value;
        return send(std::move(copy));
    }

    /** @brief 使用线程独占 producer token 复制并发送一条消息。 */
    bool send(ProducerToken& token, const T& value)
        requires std::copy_constructible<T> &&
            std::is_nothrow_copy_constructible_v<T>
    {
        T copy = value;
        return send(token, std::move(copy));
    }

    /**
     * @brief 复制发送一批消息。
     * @return 全部入队成功返回 true；失败时返回 false 且本批次未入队。
     */
    bool sendBatch(const std::vector<T>& values)
        requires std::copy_constructible<T> &&
            std::is_nothrow_copy_constructible_v<T>
    {
        return sendBatchImpl(nullptr, values.data(), values.size());
    }

    /** @brief 使用线程独占 producer token 复制发送一批消息。 */
    bool sendBatch(ProducerToken& token,
                   const std::vector<T>& values)
        requires std::copy_constructible<T> &&
            std::is_nothrow_copy_constructible_v<T>
    {
        if (!token.validFor(*this)) {
            return false;
        }
        return sendBatchImpl(&token, values.data(), values.size());
    }

    /**
     * @brief 移动发送一批消息。
     * @return 全部入队成功返回 true；失败时返回 false。
     */
    bool sendBatch(std::vector<T>&& values)
    {
        return sendBatchImpl(
            nullptr, std::make_move_iterator(values.begin()), values.size());
    }

    /** @brief 使用线程独占 producer token 移动发送一批消息。 */
    bool sendBatch(ProducerToken& token, std::vector<T>&& values)
    {
        if (!token.validFor(*this)) {
            return false;
        }
        return sendBatchImpl(
            &token, std::make_move_iterator(values.begin()), values.size());
    }

    /** @brief 尝试立即接收一条消息；为空时返回 std::nullopt。 */
    std::optional<T> tryRecv()
    {
        return tryRecvImpl(nullptr);
    }

    /**
     * @brief 使用线程独占 consumer token 尝试立即接收一条消息。
     * @return 取到消息时返回该值；队列为空或 token 不属于当前通道时返回 std::nullopt。
     */
    std::optional<T> tryRecv(ConsumerToken& token)
    {
        if (!token.validFor(*this)) {
            return std::nullopt;
        }
        return tryRecvTokenFast(token);
    }

    /** @brief 异步接收一条消息；空时挂起协程。 */
    UnboundedRecvAwaitable<T> recv()
    {
        return UnboundedRecvAwaitable<T>(this);
    }

    /** @brief 尝试接收最多 count 条消息；无消息时返回 std::nullopt。 */
    std::optional<std::vector<T>> tryRecvBatch(size_t count)
    {
        return tryRecvBatchImpl(nullptr, count);
    }

    /**
     * @brief 使用线程独占 consumer token 尝试接收最多 count 条消息。
     * @return 取到消息时返回批次；队列为空或 token 不属于当前通道时返回 std::nullopt。
     */
    std::optional<std::vector<T>> tryRecvBatch(ConsumerToken& token, size_t count)
    {
        if (!token.validFor(*this)) {
            return std::nullopt;
        }
        return tryRecvBatchImpl(&token, count);
    }

    /** @brief 异步接收最多 count 条消息；空时等待至少一条。 */
    UnboundedRecvBatchAwaitable<T> recvBatch(size_t count)
    {
        return UnboundedRecvBatchAwaitable<T>(this, count);
    }

    /**
     * @brief 关闭通道并唤醒所有等待接收者。
     * @details close 在线性化点之后拒绝新的发送许可。已经取得许可的发送仍可完成；
     *          接收方会等待这些 producer 恢复静止，二次判空并排空队列后才返回 kClosed。
     * @note 可与发送、接收及其他 close() 并发执行，且操作幂等。
     */
    void close() noexcept
    {
        const uint64_t previous =
            m_tail.fetch_or(kClosedBit, std::memory_order_acq_rel);
        if ((previous & kClosedBit) != 0) {
            return;
        }
        requestRecvPump();
    }

    /**
     * @brief 查询 close 是否已经发布。
     * @return true 表示已拒绝新的发送许可，但在先发送仍可能尚未完成入队。
     * @note 存在并发发送时，不可用 tryRecv() 为空且 isClosed() 为 true 推断 drain 完成；
     *       异步 recv()/recvBatch() 会等待 producer 静止、二次判空后返回 kClosed。
     */
    bool isClosed() const noexcept
    {
        return (m_tail.load(std::memory_order_acquire) & kClosedBit) != 0;
    }

    /** @brief 返回当前待消费消息的近似数量，仅供诊断。 */
    size_t size() const noexcept
    {
        const uint64_t tail =
            m_tail.load(std::memory_order_relaxed) & kPositionMask;
        const uint64_t head = m_head.load(std::memory_order_relaxed);
        return static_cast<size_t>(tail >= head ? tail - head : 0);
    }

    /** @brief 近似检查通道是否为空。 */
    bool empty() const noexcept
    {
        return size() == 0;
    }

private:
    using Waiter = UnboundedChannelWaiter<T>;
    using WaiterPtr = std::shared_ptr<Waiter>;
    using WaiterQueue = moodycamel::ConcurrentQueue<WaiterPtr>;

    enum class ClosedRecvProbe : uint8_t {
        kOpen,
        kValue,
        kClosed,
    };

    inline static std::atomic<uint64_t> s_generationSeed{1};

    static uint64_t nextGeneration() noexcept
    {
        uint64_t generation =
            s_generationSeed.fetch_add(1, std::memory_order_relaxed);
        if (generation == 0) {
            generation = s_generationSeed.fetch_add(1, std::memory_order_relaxed);
        }
        return generation;
    }

    static DefaultProducerCache& defaultProducerCache() noexcept
    {
        static thread_local DefaultProducerCache cache;
        return cache;
    }

    void lockBlockPool() noexcept
    {
        while (m_blockPoolLock.test_and_set(std::memory_order_acquire)) {
            detail::unboundedChannelCpuPause();
        }
    }

    void unlockBlockPool() noexcept
    {
        m_blockPoolLock.clear(std::memory_order_release);
    }

    bool blockSlotsAreFree(const Block& block) const noexcept
    {
        const uint64_t base = block.base.load(std::memory_order_acquire);
        for (uint64_t offset = 0; offset < kSlotsPerBlock; ++offset) {
            if (block.slots[static_cast<size_t>(offset)].sequence.load(
                    std::memory_order_acquire) !=
                base + offset + kSlotsPerBlock) {
                return false;
            }
        }
        return true;
    }

    Block* takeBlockLocked(uint64_t base) noexcept
    {
        Block* block = nullptr;
        Block** link = &m_retiredBlocks;
        while (*link != nullptr) {
            if (blockSlotsAreFree(**link)) {
                block = *link;
                *link = block->poolNext;
                block->poolNext = nullptr;
                break;
            }
            link = &(*link)->poolNext;
        }
        if (block == nullptr) {
            block = new (std::nothrow) Block;
            if (block == nullptr) {
                return nullptr;
            }
            block->allNext = m_allBlocks;
            m_allBlocks = block;
        }

        block->next.store(nullptr, std::memory_order_relaxed);
        block->state.store(BlockState::kActive, std::memory_order_relaxed);
        for (uint64_t offset = 0; offset < kSlotsPerBlock; ++offset) {
            block->slots[static_cast<size_t>(offset)].sequence.store(
                base + offset, std::memory_order_relaxed);
        }
        block->base.store(base, std::memory_order_release);
        return block;
    }

    Block* allocateBlock(uint64_t base) noexcept
    {
        lockBlockPool();
        Block* block = takeBlockLocked(base);
        unlockBlockPool();
        return block;
    }

    Block* ensureNextBlock(Block& block, uint64_t nextBase) noexcept
    {
        const uint64_t expectedBase = nextBase - kSlotsPerBlock;
        if (block.base.load(std::memory_order_acquire) != expectedBase ||
            block.state.load(std::memory_order_acquire) != BlockState::kActive) {
            return nullptr;
        }
        Block* next = block.next.load(std::memory_order_acquire);
        if (next != nullptr) {
            if (next->base.load(std::memory_order_acquire) != nextBase ||
                next->state.load(std::memory_order_acquire) !=
                    BlockState::kActive) {
                return nullptr;
            }
            if ((m_tail.load(std::memory_order_acquire) & kPositionMask) >=
                    nextBase ||
                m_head.load(std::memory_order_acquire) >= nextBase) {
                catchUpBlockAnchors();
            }
            return next;
        }

        lockBlockPool();
        if (block.base.load(std::memory_order_acquire) != expectedBase ||
            block.state.load(std::memory_order_acquire) != BlockState::kActive) {
            unlockBlockPool();
            return nullptr;
        }
        next = block.next.load(std::memory_order_acquire);
        if (next == nullptr) {
            next = takeBlockLocked(nextBase);
            if (next != nullptr) {
                block.next.store(next, std::memory_order_release);
            }
        } else if (next->base.load(std::memory_order_acquire) != nextBase ||
                   next->state.load(std::memory_order_acquire) !=
                       BlockState::kActive) {
            next = nullptr;
        }
        unlockBlockPool();
        if (next != nullptr) {
            if ((m_tail.load(std::memory_order_acquire) & kPositionMask) >=
                    nextBase ||
                m_head.load(std::memory_order_acquire) >= nextBase) {
                catchUpBlockAnchors();
            }
        }
        return next;
    }

    void catchUpBlockAnchors() noexcept
    {
        lockBlockPool();
        const uint64_t tailPosition =
            m_tail.load(std::memory_order_acquire) & kPositionMask;
        Block* tailBlock = m_tailBlock.load(std::memory_order_acquire);
        while (tailBlock != nullptr) {
            const uint64_t base =
                tailBlock->base.load(std::memory_order_acquire);
            if (base > kPositionMask - kSlotsPerBlock ||
                base + kSlotsPerBlock > tailPosition) {
                break;
            }
            Block* next = tailBlock->next.load(std::memory_order_acquire);
            if (next == nullptr ||
                next->base.load(std::memory_order_acquire) !=
                    base + kSlotsPerBlock ||
                next->state.load(std::memory_order_acquire) !=
                    BlockState::kActive) {
                break;
            }
            tailBlock = next;
            m_tailBlock.store(tailBlock, std::memory_order_release);
        }

        const uint64_t headPosition = m_head.load(std::memory_order_acquire);
        Block* headBlock = m_headBlock.load(std::memory_order_acquire);
        while (headBlock != nullptr) {
            const uint64_t base =
                headBlock->base.load(std::memory_order_acquire);
            if (base > kPositionMask - kSlotsPerBlock ||
                base + kSlotsPerBlock > headPosition) {
                break;
            }
            Block* next = headBlock->next.load(std::memory_order_acquire);
            if (next == nullptr ||
                next->base.load(std::memory_order_acquire) !=
                    base + kSlotsPerBlock ||
                next->state.load(std::memory_order_acquire) !=
                    BlockState::kActive) {
                break;
            }
            m_headBlock.store(next, std::memory_order_release);
            if (m_tailBlock.load(std::memory_order_acquire) != headBlock &&
                headBlock->state.load(std::memory_order_acquire) ==
                    BlockState::kActive) {
                headBlock->state.store(BlockState::kRetired,
                                       std::memory_order_release);
                headBlock->poolNext = m_retiredBlocks;
                m_retiredBlocks = headBlock;
            }
            headBlock = next;
        }
        unlockBlockPool();
    }

#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline, cold))
#endif
    Block* tailBlockFor(Block*& cached,
                        uint64_t& cachedBase,
                        uint64_t position) noexcept
    {
        const uint64_t targetBase =
            position & ~(kSlotsPerBlock - 1);
        if (cached != nullptr && cachedBase == targetBase &&
            cached->base.load(std::memory_order_acquire) == targetBase &&
            cached->state.load(std::memory_order_acquire) == BlockState::kActive) {
            return cached;
        }

        Block* block = m_tailBlock.load(std::memory_order_acquire);
        while (block != nullptr) {
            const uint64_t base = block->base.load(std::memory_order_acquire);
            if (base == targetBase &&
                block->state.load(std::memory_order_acquire) ==
                    BlockState::kActive) {
                cached = block;
                cachedBase = base;
                return block;
            }
            if (base > targetBase || base > kPositionMask - kSlotsPerBlock) {
                return nullptr;
            }
            Block* next = ensureNextBlock(*block, base + kSlotsPerBlock);
            if (next == nullptr) {
                return nullptr;
            }
            block = next;
        }
        return nullptr;
    }

#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline, cold))
#endif
    Block* headBlockFor(Block*& cached,
                        uint64_t& cachedBase,
                        uint64_t position) noexcept
    {
        const uint64_t targetBase =
            position & ~(kSlotsPerBlock - 1);
        if (cached != nullptr && cachedBase == targetBase &&
            cached->base.load(std::memory_order_acquire) == targetBase &&
            cached->state.load(std::memory_order_acquire) == BlockState::kActive) {
            return cached;
        }

        Block* block = m_headBlock.load(std::memory_order_acquire);
        while (block != nullptr) {
            const uint64_t base = block->base.load(std::memory_order_acquire);
            if (base == targetBase &&
                block->state.load(std::memory_order_acquire) ==
                    BlockState::kActive) {
                cached = block;
                cachedBase = base;
                return block;
            }
            if (base > targetBase) {
                return nullptr;
            }
            Block* next = block->next.load(std::memory_order_acquire);
            if (next == nullptr) {
                return nullptr;
            }
            block = next;
        }
        return nullptr;
    }

    DefaultProducerCacheEntry* defaultProducerEntry() noexcept
    {
        DefaultProducerCache& cache = defaultProducerCache();
        DefaultProducerCacheEntry** link = &cache.head;
        while (*link != nullptr) {
            DefaultProducerCacheEntry* entry = *link;
            if (entry->channel == this && entry->generation == m_generation) {
                if (link != &cache.head) {
                    *link = entry->next;
                    entry->next = cache.head;
                    cache.head = entry;
                }
                return entry;
            }
            link = &entry->next;
        }

        auto* entry = new (std::nothrow) DefaultProducerCacheEntry;
        if (entry == nullptr) {
            return nullptr;
        }
        entry->channel = this;
        entry->generation = m_generation;
        entry->block = m_tailBlock.load(std::memory_order_acquire);
        entry->blockBase = entry->block != nullptr
            ? entry->block->base.load(std::memory_order_acquire)
            : kInvalidBlockBase;
        entry->next = cache.head;
        cache.head = entry;
        return entry;
    }

    bool reserveRange(ProducerToken* token,
                      size_t count,
                      Block*& firstBlock,
                      uint64_t& firstPosition) noexcept
    {
        DefaultProducerCacheEntry* entry =
            token == nullptr ? defaultProducerEntry() : nullptr;
        if (token == nullptr && entry == nullptr) {
            return false;
        }
        Block*& cachedBlock = token != nullptr ? token->m_block : entry->block;
        uint64_t& cachedBase =
            token != nullptr ? token->m_blockBase : entry->blockBase;

        uint64_t tail = m_tail.load(std::memory_order_relaxed);
        for (;;) {
            if ((tail & kClosedBit) != 0) {
                return false;
            }
            const uint64_t position = tail;
            if (count > kPositionMask - position) {
                return false;
            }
            Block* first = tailBlockFor(cachedBlock, cachedBase, position);
            if (first == nullptr) {
                const uint64_t latest =
                    m_tail.load(std::memory_order_relaxed);
                if (latest != tail) {
                    tail = latest;
                    detail::unboundedChannelCpuPause();
                    continue;
                }
                return false;
            }
            Block* last = first;
            const uint64_t lastPosition = position + count - 1;
            uint64_t lastBase = first->base.load(std::memory_order_acquire);
            while (lastPosition >= lastBase + kSlotsPerBlock) {
                last = ensureNextBlock(*last, lastBase + kSlotsPerBlock);
                if (last == nullptr) {
                    const uint64_t latest =
                        m_tail.load(std::memory_order_relaxed);
                    if (latest != tail) {
                        tail = latest;
                        detail::unboundedChannelCpuPause();
                        last = nullptr;
                        break;
                    }
                    return false;
                }
                lastBase += kSlotsPerBlock;
            }
            if (last == nullptr) {
                continue;
            }

            if (m_tail.compare_exchange_weak(tail,
                                             position + count,
                                             std::memory_order_relaxed,
                                             std::memory_order_relaxed)) {
                firstBlock = first;
                firstPosition = position;
                cachedBlock = last;
                cachedBase = lastBase;
                catchUpBlockAnchors();
                return true;
            }
            detail::unboundedChannelCpuPause();
        }
    }

    /** @brief 发布入队完成后的 waiter work。 */
    void releaseSendPublication() noexcept
    {
        if (waiterPathUsedAfterPublish()) [[unlikely]] {
            requestRecvPumpAfterSend();
        }
    }

    // 同步 tryRecv 压测中 waiter 路径从未启用；把完整 pump 状态机留在冷函数，
    // 避免每条 send 的公共热路径因内联冷分支而膨胀。
#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline, cold))
#endif
    void requestRecvPumpAfterSend() noexcept
    {
        requestRecvPump();
    }

    /** @brief 仅当 close 已发布且所有保留位置均已被 consumer claim 时返回 true。 */
    bool sendSideQuiescentAfterClose() const noexcept
    {
        const uint64_t tail = m_tail.load(std::memory_order_acquire);
        return (tail & kClosedBit) != 0 &&
            (tail & kPositionMask) == m_head.load(std::memory_order_acquire);
    }

    /**
     * @brief 首次 dequeue 为空后，在线性化关闭前执行 producer 扫描与二次 dequeue。
     * @details producer 可能在首次空检查后完成 enqueue 并恢复为 idle；只有
     *          quiescent scan 后的第二次 dequeue 仍为空，接收方才可发布 Closed。
     */
    ClosedRecvProbe probeRecvAfterEmpty(T& value) noexcept
    {
        if (auto received = tryRecvImpl(nullptr); received.has_value()) {
            value = std::move(*received);
            return ClosedRecvProbe::kValue;
        }
        return sendSideQuiescentAfterClose()
            ? ClosedRecvProbe::kClosed
            : ClosedRecvProbe::kOpen;
    }

#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline, cold))
#endif
    void prepareNextBlockAfterReservation(Block& block,
                                          uint64_t blockBase) noexcept
    {
        if (blockBase <= kPositionMask - kSlotsPerBlock) {
            Block* prepared =
                ensureNextBlock(block, blockBase + kSlotsPerBlock);
            if (prepared == nullptr) {
                // 当前 reservation 已成功；预取失败由下次 rollover 重试。
            }
        }
        catchUpBlockAnchors();
    }

    template <bool NotifyWaiter>
#if defined(_MSC_VER)
    __forceinline
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((always_inline)) inline
#else
    inline
#endif
    bool sendTokenFast(ProducerToken& token, T&& value)
    {
        uint64_t tail = m_tail.load(std::memory_order_relaxed);
        uint32_t backoffStep = 0;
        for (;;) {
            if ((tail & kClosedBit) != 0 || tail == kPositionMask) {
                return false;
            }
            const uint64_t position = tail;
            const uint64_t blockBase =
                position & ~(kSlotsPerBlock - 1);
            Block* block = token.m_block;
            if (block == nullptr || token.m_blockBase != blockBase ||
                block->base.load(std::memory_order_acquire) != blockBase ||
                block->state.load(std::memory_order_acquire) !=
                    BlockState::kActive) {
                block = tailBlockFor(token.m_block, token.m_blockBase, position);
                if (block == nullptr) {
                    const uint64_t latest =
                        m_tail.load(std::memory_order_relaxed);
                    if (latest != tail) {
                        tail = latest;
                        detail::unboundedChannelBackoff(backoffStep);
                        continue;
                    }
                    return false;
                }
            }
            Slot& slot = block->slots[static_cast<size_t>(
                position & (kSlotsPerBlock - 1))];
            if (slot.sequence.load(std::memory_order_acquire) != position) {
                tail = m_tail.load(std::memory_order_relaxed);
                detail::unboundedChannelBackoff(backoffStep);
                continue;
            }
            if (!m_tail.compare_exchange_weak(tail,
                                              position + 1,
                                              std::memory_order_relaxed,
                                              std::memory_order_relaxed)) {
                detail::unboundedChannelBackoff(backoffStep);
                continue;
            }
            std::construct_at(slot.value(), std::move(value));
            slot.sequence.store(position + 1, std::memory_order_release);
            if constexpr (NotifyWaiter) {
                releaseSendPublication();
            }
            if ((position & (kSlotsPerBlock - 1)) == 0) {
                prepareNextBlockAfterReservation(*block, blockBase);
            }
            return true;
        }
    }

#if defined(_MSC_VER)
    __forceinline
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((always_inline)) inline
#else
    inline
#endif
    std::optional<T> tryRecvTokenFast(ConsumerToken& token)
    {
        uint64_t position = m_head.load(std::memory_order_relaxed);
        uint32_t backoffStep = 0;
        for (;;) {
            const uint64_t blockBase =
                position & ~(kSlotsPerBlock - 1);
            Block* block = token.m_block;
            if (block == nullptr || token.m_blockBase != blockBase ||
                block->base.load(std::memory_order_acquire) != blockBase ||
                block->state.load(std::memory_order_acquire) !=
                    BlockState::kActive) {
                block = headBlockFor(token.m_block, token.m_blockBase, position);
                if (block == nullptr) {
                    return std::nullopt;
                }
            }
            Slot& slot = block->slots[static_cast<size_t>(
                position & (kSlotsPerBlock - 1))];
            const uint64_t sequence =
                slot.sequence.load(std::memory_order_acquire);
            if (sequence == position + 1) {
                if (!m_head.compare_exchange_weak(position,
                                                   position + 1,
                                                   std::memory_order_relaxed,
                                                   std::memory_order_relaxed)) {
                    detail::unboundedChannelBackoff(backoffStep);
                    continue;
                }
                if ((position & (kSlotsPerBlock - 1)) ==
                    kSlotsPerBlock - 1) {
                    catchUpBlockAnchors();
                }
                T value = std::move(*slot.value());
                std::destroy_at(slot.value());
                slot.sequence.store(position + kSlotsPerBlock,
                                    std::memory_order_release);
                return value;
            }
            if (sequence < position + 1) {
                return std::nullopt;
            }
            position = m_head.load(std::memory_order_relaxed);
            detail::unboundedChannelBackoff(backoffStep);
        }
    }

#if defined(_MSC_VER)
    __forceinline
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((always_inline)) inline
#else
    inline
#endif
    bool sendImpl(ProducerToken* token, T&& value)
    {
        DefaultProducerCacheEntry* entry =
            token == nullptr ? defaultProducerEntry() : nullptr;
        if (token == nullptr && entry == nullptr) {
            return false;
        }
        Block*& cachedBlock = token != nullptr ? token->m_block : entry->block;
        uint64_t& cachedBase =
            token != nullptr ? token->m_blockBase : entry->blockBase;

        uint64_t tail = m_tail.load(std::memory_order_relaxed);
        Block* block = nullptr;
        uint64_t position = 0;
        uint64_t blockBase = 0;
        for (;;) {
            if ((tail & kClosedBit) != 0) {
                return false;
            }
            position = tail;
            if (position == kPositionMask) {
                return false;
            }
            blockBase = position & ~(kSlotsPerBlock - 1);
            block = cachedBlock;
            if (block == nullptr || cachedBase != blockBase ||
                block->base.load(std::memory_order_acquire) != blockBase ||
                block->state.load(std::memory_order_acquire) !=
                    BlockState::kActive) {
                block = tailBlockFor(cachedBlock, cachedBase, position);
                if (block == nullptr) {
                    const uint64_t latest =
                        m_tail.load(std::memory_order_relaxed);
                    if (latest != tail) {
                        tail = latest;
                        detail::unboundedChannelCpuPause();
                        continue;
                    }
                    return false;
                }
            }
            if (m_tail.compare_exchange_weak(tail,
                                             position + 1,
                                             std::memory_order_relaxed,
                                             std::memory_order_relaxed)) {
                break;
            }
            detail::unboundedChannelCpuPause();
        }
        Slot& slot = block->slots[static_cast<size_t>(
            position & (kSlotsPerBlock - 1))];
        std::construct_at(slot.value(), std::move(value));
        slot.sequence.store(position + 1, std::memory_order_release);
        releaseSendPublication();
        if ((position & (kSlotsPerBlock - 1)) == 0) {
            prepareNextBlockAfterReservation(*block, blockBase);
        }
        return true;
    }

    template <typename Iterator>
    bool sendBatchImpl(ProducerToken* token, Iterator first, size_t count)
    {
        if (count == 0) {
            return true;
        }
        Block* block = nullptr;
        uint64_t position = 0;
        if (!reserveRange(token, count, block, position)) {
            return false;
        }

        for (size_t index = 0; index < count; ++index, ++first, ++position) {
            const uint64_t expectedBase =
                position & ~(kSlotsPerBlock - 1);
            if (block->base.load(std::memory_order_acquire) != expectedBase) {
                block = block->next.load(std::memory_order_acquire);
            }
            T value = *first;
            Slot& slot = block->slots[static_cast<size_t>(
                position & (kSlotsPerBlock - 1))];
            std::construct_at(slot.value(), std::move(value));
            slot.sequence.store(position + 1, std::memory_order_release);
        }
        releaseSendPublication();
        return true;
    }

#if defined(_MSC_VER)
    __forceinline
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((always_inline)) inline
#else
    inline
#endif
    std::optional<T> tryRecvImpl(ConsumerToken* token)
    {
        Block* uncached = nullptr;
        uint64_t uncachedBase = kInvalidBlockBase;
        Block*& cachedBlock = token != nullptr ? token->m_block : uncached;
        uint64_t& cachedBase =
            token != nullptr ? token->m_blockBase : uncachedBase;
        uint64_t position = m_head.load(std::memory_order_relaxed);
        for (;;) {
            Block* block = headBlockFor(cachedBlock, cachedBase, position);
            if (block == nullptr) {
                return std::nullopt;
            }
            Slot& slot = block->slots[static_cast<size_t>(
                position & (kSlotsPerBlock - 1))];
            const uint64_t expected = position + 1;
            const uint64_t sequence =
                slot.sequence.load(std::memory_order_acquire);
            if (sequence == expected) {
                if (!m_head.compare_exchange_weak(position,
                                                   position + 1,
                                                   std::memory_order_relaxed,
                                                   std::memory_order_relaxed)) {
                    detail::unboundedChannelCpuPause();
                    continue;
                }
                if ((position & (kSlotsPerBlock - 1)) ==
                    kSlotsPerBlock - 1) {
                    catchUpBlockAnchors();
                }
                T value = std::move(*slot.value());
                std::destroy_at(slot.value());
                slot.sequence.store(position + kSlotsPerBlock,
                                    std::memory_order_release);
                return value;
            }
            if (sequence < expected) {
                return std::nullopt;
            }
            position = m_head.load(std::memory_order_relaxed);
            detail::unboundedChannelCpuPause();
        }
    }

    std::optional<std::vector<T>> tryRecvBatchImpl(ConsumerToken* token, size_t count)
    {
        if (count == 0) {
            return std::vector<T>{};
        }
        std::vector<T> values;
        values.resize(count);
        size_t received = 0;
        while (received < count) {
            auto value = tryRecvImpl(token);
            if (!value.has_value()) {
                break;
            }
            values[received] = std::move(*value);
            ++received;
        }
        if (received == 0) {
            return std::nullopt;
        }
        values.resize(received);
        return values;
    }

    bool claimWaiter(const WaiterPtr& waiter) noexcept
    {
        UnboundedWaiterState expected = UnboundedWaiterState::kWaiting;
        return waiter->state.compare_exchange_strong(
            expected,
            UnboundedWaiterState::kFulfilling,
            std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

    bool enqueueWaiter(const WaiterPtr& waiter) noexcept
    {
        bool expected = false;
        if (!waiter->queued.compare_exchange_strong(expected,
                                                    true,
                                                    std::memory_order_acq_rel,
                                                    std::memory_order_acquire)) {
            return true;
        }
        if (m_recvWaiters.enqueue(waiter)) {
            return true;
        }
        waiter->queued.store(false, std::memory_order_release);
        return false;
    }

    bool tryDequeueWaiter(WaiterPtr& waiter) noexcept
    {
        while (m_recvWaiters.try_dequeue(waiter)) {
            if (waiter && waiter->queued.exchange(false, std::memory_order_acq_rel)) {
                return true;
            }
        }
        return false;
    }

    bool waiterPathUsedAfterPublish() noexcept
    {
        // 读到 1 时由 producer 请求 pump；读到 0 的并发注册方会在发布 waiter 后
        // 无条件请求 pump，因此两种顺序都不会漏掉已入队消息。
        return (m_recvWaiterPathUsed.load(std::memory_order_seq_cst) &
                kRecvWaiterPathUsed) != 0;
    }

    void registerRecvWaiterPath() noexcept
    {
        // waiter 已先进入等待队列；生产者读到 1 时会请求 recv pump 推进。
        uint8_t observed = m_recvWaiterPathUsed.load(std::memory_order_seq_cst);
        while ((observed & kRecvWaiterPathUsed) == 0 &&
               !m_recvWaiterPathUsed.compare_exchange_weak(
                   observed,
                   static_cast<uint8_t>(observed | kRecvWaiterPathUsed),
                   std::memory_order_seq_cst,
                   std::memory_order_seq_cst)) {
            detail::unboundedChannelCpuPause();
        }
    }

    bool takeRecvCleanupRequest() noexcept
    {
        uint8_t observed = m_recvWaiterPathUsed.load(std::memory_order_acquire);
        while ((observed & kRecvCleanupPending) != 0) {
            const uint8_t desired =
                static_cast<uint8_t>(observed & ~kRecvCleanupPending);
            if (m_recvWaiterPathUsed.compare_exchange_weak(
                    observed,
                    desired,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }

    void wakeWaiter(Waiter& waiter) noexcept
    {
        if (!waiter.completionWaker->requestWake()) {
            // timeout 或其他完成路径已经发布同一 waiter 的唤醒。
        }
    }

    UnboundedWaiterProgress tryCompleteRecvWaiter(const WaiterPtr& waiter) noexcept
    {
        if (!waiter) {
            return UnboundedWaiterProgress::kSkipped;
        }

        TimeoutTimer* const timeoutTimer = waiter->timeoutTimer.get();
        bool timeoutOperationStarted = false;
        if (timeoutTimer != nullptr) {
            const auto start = timeoutTimer->tryBeginOperation();
            if (start == TimeoutTimer::OperationStart::kBusy) {
                const auto state = waiter->state.load(std::memory_order_acquire);
                if (state == UnboundedWaiterState::kWaiting &&
                    enqueueWaiter(waiter)) {
                    return UnboundedWaiterProgress::kBlocked;
                }
                return UnboundedWaiterProgress::kSkipped;
            }
            if (start == TimeoutTimer::OperationStart::kTimeoutWon) {
                UnboundedWaiterState expected = UnboundedWaiterState::kWaiting;
                if (!waiter->state.compare_exchange_strong(
                        expected,
                        UnboundedWaiterState::kCancelled,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    // timeout winner 或当前 operation owner 会发布最终状态。
                }
                return UnboundedWaiterProgress::kSkipped;
            }
            if (start == TimeoutTimer::OperationStart::kOperationWon) {
                return UnboundedWaiterProgress::kSkipped;
            }
            timeoutOperationStarted = true;
        }

        if (!claimWaiter(waiter)) {
            if (timeoutOperationStarted) {
                const auto aborted = timeoutTimer->abortOperation();
                if (aborted == TimeoutTimer::OperationAbort::kTimeoutWon) {
                    waiter->state.store(UnboundedWaiterState::kCancelled,
                                        std::memory_order_release);
                    timeoutTimer->wakeTimeoutWinner();
                    return UnboundedWaiterProgress::kSkipped;
                }
            }
            return UnboundedWaiterProgress::kSkipped;
        }

        T value;
        auto received = tryRecvImpl(nullptr);
        bool dequeued = received.has_value();
        if (dequeued) {
            value = std::move(*received);
        }
        ClosedRecvProbe closedProbe = ClosedRecvProbe::kOpen;
        if (!dequeued) {
            closedProbe = probeRecvAfterEmpty(value);
            dequeued = closedProbe == ClosedRecvProbe::kValue;
        }
        if (dequeued) {
            waiter->value.emplace(std::move(value));
            const bool completionCommitted =
                !timeoutOperationStarted || timeoutTimer->commitOperation();
            waiter->state.store(completionCommitted
                                    ? UnboundedWaiterState::kFulfilled
                                    : UnboundedWaiterState::kFailed,
                                std::memory_order_release);
            wakeWaiter(*waiter);
            return UnboundedWaiterProgress::kProgressed;
        }

        if (closedProbe == ClosedRecvProbe::kClosed) {
            const bool completionCommitted =
                !timeoutOperationStarted || timeoutTimer->commitOperation();
            waiter->state.store(completionCommitted
                                    ? UnboundedWaiterState::kClosed
                                    : UnboundedWaiterState::kFailed,
                                std::memory_order_release);
            wakeWaiter(*waiter);
            return UnboundedWaiterProgress::kProgressed;
        }

        if (!enqueueWaiter(waiter)) {
            const bool completionCommitted =
                !timeoutOperationStarted || timeoutTimer->commitOperation();
            waiter->state.store(UnboundedWaiterState::kFailed,
                                std::memory_order_release);
            if (!completionCommitted) {
                // kFailed 已是唯一可传播的终态，不再覆盖。
            }
            wakeWaiter(*waiter);
            return UnboundedWaiterProgress::kProgressed;
        }
        waiter->state.store(UnboundedWaiterState::kWaiting, std::memory_order_release);
        if (!timeoutOperationStarted) {
            return UnboundedWaiterProgress::kBlocked;
        }

        const auto aborted = timeoutTimer->abortOperation();
        if (aborted == TimeoutTimer::OperationAbort::kTimeoutWon) {
            waiter->state.store(UnboundedWaiterState::kCancelled,
                                std::memory_order_release);
            timeoutTimer->wakeTimeoutWinner();
            return UnboundedWaiterProgress::kSkipped;
        }
        if (aborted == TimeoutTimer::OperationAbort::kCompleted) {
            return UnboundedWaiterProgress::kSkipped;
        }
        return UnboundedWaiterProgress::kBlocked;
    }

    void drainRecvWaiters(bool cleanupRequested) noexcept
    {
        Waiter* blockedBoundary = nullptr;
        WaiterPtr waiter;
        while (tryDequeueWaiter(waiter)) {
            const bool reachedBoundary = waiter.get() == blockedBoundary;
            const auto progress = tryCompleteRecvWaiter(waiter);
            if (progress != UnboundedWaiterProgress::kBlocked) {
                if (reachedBoundary) {
                    blockedBoundary = nullptr;
                }
                continue;
            }
            if (reachedBoundary) {
                return;
            }
            if (!cleanupRequested) {
                return;
            }
            if (blockedBoundary == nullptr) {
                // 无资源时完整轮转一次，清理排在 live waiter 后面的 timeout tombstone。
                blockedBoundary = waiter.get();
            }
        }
    }

    /** @brief 由唯一 owner 排空已发布的 recv work，直到退出 CAS 确认没有新事件。 */
    void runRecvPump() noexcept
    {
        for (;;) {
            const uint8_t claimed =
                m_recvPumpState.fetch_and(kPumpRunning, std::memory_order_acq_rel);
            if ((claimed & kRecvWork) != 0) {
                drainRecvWaiters(takeRecvCleanupRequest());
            }

            uint8_t expected = kPumpRunning;
            if (m_recvPumpState.compare_exchange_strong(
                    expected,
                    0,
                    std::memory_order_release,
                    std::memory_order_acquire)) {
                return;
            }
        }
    }

    /**
     * @brief 发布一次 recv work；没有现存 owner 时由当前线程同步取得 pump 所有权。
     * @note Running 与 RecvWork 共用一个原子，owner 退出 CAS 与并发请求互斥，
     *       因而 requester 观察到 Running 后可以直接返回而不会丢事件。
     */
    void requestRecvPump() noexcept
    {
        uint8_t observed =
            m_recvPumpState.fetch_or(kRecvWork, std::memory_order_release);
        for (;;) {
            if ((observed & kPumpRunning) != 0) {
                return;
            }
            observed = static_cast<uint8_t>(observed | kRecvWork);
            const uint8_t desired =
                static_cast<uint8_t>(observed | kPumpRunning);
            if (m_recvPumpState.compare_exchange_weak(
                    observed,
                    desired,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                runRecvPump();
                return;
            }
        }
    }

    void requestRecvCleanup() noexcept
    {
        uint8_t observed = m_recvWaiterPathUsed.load(std::memory_order_relaxed);
        while ((observed & kRecvCleanupPending) == 0 &&
               !m_recvWaiterPathUsed.compare_exchange_weak(
                   observed,
                   static_cast<uint8_t>(observed | kRecvCleanupPending),
                   std::memory_order_release,
                   std::memory_order_relaxed)) {
            detail::unboundedChannelCpuPause();
        }
        requestRecvPump();
    }

    friend class UnboundedRecvAwaitable<T>;
    friend class UnboundedRecvBatchAwaitable<T>;
    friend struct UnboundedChannelTestAccess;

    alignas(::galay::utils::kCacheLineSize) std::atomic<uint64_t> m_tail{0};
    alignas(::galay::utils::kCacheLineSize) std::atomic<uint64_t> m_head{0};
    alignas(::galay::utils::kCacheLineSize)
        std::atomic<Block*> m_tailBlock{nullptr};
    alignas(::galay::utils::kCacheLineSize)
        std::atomic<Block*> m_headBlock{nullptr};
    alignas(::galay::utils::kCacheLineSize)
        std::atomic<uint8_t> m_recvPumpState{0};
    alignas(::galay::utils::kCacheLineSize)
        std::atomic<uint8_t> m_recvWaiterPathUsed{0};
    std::atomic_flag m_blockPoolLock = ATOMIC_FLAG_INIT;
    Block* m_retiredBlocks = nullptr;
    Block* m_allBlocks = nullptr;
    WaiterQueue m_recvWaiters;
    const uint64_t m_generation;
};

template <UnboundedValue T>
inline bool UnboundedRecvAwaitable<T>::await_ready() noexcept
{
    if (auto value = m_channel->tryRecv(); value.has_value()) {
        m_ready.emplace(std::move(*value));
        return true;
    }
    T value;
    const auto probe = m_channel->probeRecvAfterEmpty(value);
    if (probe == UnboundedChannel<T>::ClosedRecvProbe::kValue) {
        m_ready.emplace(std::move(value));
        return true;
    }
    return probe == UnboundedChannel<T>::ClosedRecvProbe::kClosed;
}

template <UnboundedValue T>
template <typename Promise>
inline bool UnboundedRecvAwaitable<T>::await_suspend(
    std::coroutine_handle<Promise> handle) noexcept
{
    auto* channel = m_channel;
    if (auto value = channel->tryRecv(); value.has_value()) {
        m_ready.emplace(std::move(*value));
        return false;
    }

    TimeoutTimer::ptr timeoutTimer = std::move(m_timeoutTimer);
    auto waiter = std::make_shared<UnboundedChannelWaiter<T>>(
        Waker(handle), std::move(timeoutTimer));
    m_waiter = waiter;
    if (!channel->enqueueWaiter(waiter)) {
        waiter->state.store(UnboundedWaiterState::kFailed, std::memory_order_release);
        return false;
    }
    channel->registerRecvWaiterPath();
    channel->requestRecvPump();
    if (waiter->timeoutTimer != nullptr) {
        // WithTimeout 在 timer 注册完成后统一发布共享 completion gate。
        return true;
    }
    // arm() 后完成方可能立即恢复并销毁 awaiter/channel，因此必须直接返回。
    return waiter->completionWaker->arm();
}

template <UnboundedValue T>
inline std::expected<T, IOError> UnboundedRecvAwaitable<T>::await_resume() noexcept
{
    if (m_ready.has_value()) {
        return std::move(*m_ready);
    }
    if (m_waiter) {
        detail::waitForUnboundedChannelFulfillment(*m_waiter);
        const auto state = m_waiter->state.load(std::memory_order_acquire);
        m_waiter->completionWaker->clearWaker();
        if (state == UnboundedWaiterState::kFulfilled && m_waiter->value.has_value()) {
            T value = std::move(*m_waiter->value);
            m_waiter->value.reset();
            return value;
        }
        if (state == UnboundedWaiterState::kClosed) {
            return std::unexpected(IOError(kClosed, 0));
        }
        if (state == UnboundedWaiterState::kFailed) {
            return std::unexpected(IOError(kNotReady, 0));
        }
    }
    if (m_timedOut) {
        return std::unexpected(IOError(kTimeout, 0));
    }
    if (auto value = m_channel->tryRecv(); value.has_value()) {
        return std::move(*value);
    }
    T value;
    const auto probe = m_channel->probeRecvAfterEmpty(value);
    if (probe == UnboundedChannel<T>::ClosedRecvProbe::kValue) {
        return std::move(value);
    }
    return std::unexpected(IOError(
        probe == UnboundedChannel<T>::ClosedRecvProbe::kClosed
            ? kClosed
            : kTimeout,
        0));
}

template <UnboundedValue T>
inline void UnboundedRecvAwaitable<T>::markTimeout() noexcept
{
    m_timedOut = true;
    if (!m_waiter) {
        return;
    }
    UnboundedWaiterState expected = UnboundedWaiterState::kWaiting;
    if (m_waiter->state.compare_exchange_strong(expected,
                                                UnboundedWaiterState::kCancelled,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
        m_waiter->completionWaker->clearWaker();
        m_channel->requestRecvCleanup();
    } else if (expected == UnboundedWaiterState::kCancelled) {
        m_waiter->completionWaker->clearWaker();
    }
}

template <UnboundedValue T>
inline bool UnboundedRecvBatchAwaitable<T>::await_ready() noexcept
{
    if (auto values = m_channel->tryRecvBatch(m_count); values.has_value()) {
        m_ready.emplace(std::move(*values));
        return true;
    }
    if (!m_channel->sendSideQuiescentAfterClose()) {
        return false;
    }
    if (auto values = m_channel->tryRecvBatch(m_count); values.has_value()) {
        m_ready.emplace(std::move(*values));
    }
    return true;
}

template <UnboundedValue T>
template <typename Promise>
inline bool UnboundedRecvBatchAwaitable<T>::await_suspend(
    std::coroutine_handle<Promise> handle) noexcept
{
    auto* channel = m_channel;
    if (auto values = channel->tryRecvBatch(m_count); values.has_value()) {
        m_ready.emplace(std::move(*values));
        return false;
    }

    TimeoutTimer::ptr timeoutTimer = std::move(m_timeoutTimer);
    auto waiter = std::make_shared<UnboundedChannelWaiter<T>>(
        Waker(handle), std::move(timeoutTimer));
    m_waiter = waiter;
    if (!channel->enqueueWaiter(waiter)) {
        waiter->state.store(UnboundedWaiterState::kFailed, std::memory_order_release);
        return false;
    }
    channel->registerRecvWaiterPath();
    channel->requestRecvPump();
    if (waiter->timeoutTimer != nullptr) {
        return true;
    }
    return waiter->completionWaker->arm();
}

template <UnboundedValue T>
inline std::expected<std::vector<T>, IOError>
UnboundedRecvBatchAwaitable<T>::await_resume() noexcept
{
    if (m_ready.has_value()) {
        return std::move(*m_ready);
    }
    if (m_waiter) {
        detail::waitForUnboundedChannelFulfillment(*m_waiter);
        const auto state = m_waiter->state.load(std::memory_order_acquire);
        m_waiter->completionWaker->clearWaker();
        if (state == UnboundedWaiterState::kFulfilled && m_waiter->value.has_value()) {
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
            return values;
        }
        if (state == UnboundedWaiterState::kClosed) {
            return std::unexpected(IOError(kClosed, 0));
        }
        if (state == UnboundedWaiterState::kFailed) {
            return std::unexpected(IOError(kNotReady, 0));
        }
    }
    if (m_timedOut) {
        return std::unexpected(IOError(kTimeout, 0));
    }
    if (auto values = m_channel->tryRecvBatch(m_count); values.has_value()) {
        return std::move(*values);
    }
    if (!m_channel->sendSideQuiescentAfterClose()) {
        return std::unexpected(IOError(kTimeout, 0));
    }
    if (auto values = m_channel->tryRecvBatch(m_count); values.has_value()) {
        return std::move(*values);
    }
    return std::unexpected(IOError(kClosed, 0));
}

template <UnboundedValue T>
inline void UnboundedRecvBatchAwaitable<T>::markTimeout() noexcept
{
    m_timedOut = true;
    if (!m_waiter) {
        return;
    }
    UnboundedWaiterState expected = UnboundedWaiterState::kWaiting;
    if (m_waiter->state.compare_exchange_strong(expected,
                                                UnboundedWaiterState::kCancelled,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
        m_waiter->completionWaker->clearWaker();
        m_channel->requestRecvCleanup();
    } else if (expected == UnboundedWaiterState::kCancelled) {
        m_waiter->completionWaker->clearWaker();
    }
}

} // namespace galay::mpmc

#endif // GALAY_CONCURRENCY_MPMC_UNBOUNDED_CHANNEL_H
