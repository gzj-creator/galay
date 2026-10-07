/**
 * @file unbounded_channel.h
 * @brief 多生产者单消费者无界异步通道
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 每个生产者独占一条分块 SPSC 流，唯一消费者按固定配额轮询所有流。
 * 生产者热路径没有共享 tail CAS，消费者热路径没有 cursor CAS/RMW；只有跨约
 * 4 KiB 分块时才分配或回收内存。显式 ProducerToken 直接绑定流，默认 send()
 * 使用带 channel generation 的线程本地缓存，避免同地址重建复用悬空流。
 * producer 退出后其流会成为可复用的高水位池节点，registry 大小只随峰值并发
 * producer 数增长，而不会随 producer 生命周期总数增长。
 */

#ifndef GALAY_MPSC_UNBOUNDED_CHANNEL_H
#define GALAY_MPSC_UNBOUNDED_CHANNEL_H

#include "../../common/error.h"
#include "../../core/task.h"
#include "../../core/timeout.hpp"
#include "../../core/wait_registration.h"
#include "../../core/waker.h"
#include "../../../galay-utils/common/defn.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <concepts>
#include <coroutine>
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

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <emmintrin.h>
#endif

namespace galay::mpsc
{

using kernel::IOError;
using kernel::TaskRef;
using kernel::TaskState;
using kernel::TimeoutTimer;
using kernel::TimeoutSupport;
using kernel::WaitRegistration;
using kernel::Waker;
using kernel::WithTimeout;
using kernel::kClosed;
using kernel::kParamInvalid;
using kernel::kTimeout;

/**
 * @brief 约束 UnboundedChannel 可接受的元素类型。
 * @tparam T 元素类型；只要求可移动，不要求默认构造。
 * @note T 的复制、移动构造和析构不得重入或销毁正在操作它的同一 channel；
 *       这些操作发生在 producer gate 或 consumer cursor 更新区间内。
 */
template <typename T>
concept UnboundedValue = std::movable<T> &&
    std::is_nothrow_move_constructible_v<T> &&
    std::is_nothrow_destructible_v<T>;

template <UnboundedValue T>
class UnboundedChannel;

struct UnboundedChannelTestAccess;

/** @brief 单条异步接收等待体；空时挂起协程而不阻塞线程。 */
template <UnboundedValue T>
class UnboundedRecvAwaitable : public TimeoutSupport<UnboundedRecvAwaitable<T>>
{
public:
    UnboundedRecvAwaitable(const UnboundedRecvAwaitable&) = delete;
    UnboundedRecvAwaitable& operator=(const UnboundedRecvAwaitable&) = delete;

    /**
     * @brief 在 await 协议开始前转移 awaiter 状态。
     * @note await_ready() 或 await_suspend() 任一开始后不得再移动 awaiter。
     */
    UnboundedRecvAwaitable(UnboundedRecvAwaitable&& other) noexcept
        : m_channel(std::exchange(other.m_channel, nullptr))
        , m_timeoutTimer(std::move(other.m_timeoutTimer))
        , m_waiterState(std::exchange(other.m_waiterState, nullptr))
        , m_timedOut(std::exchange(other.m_timedOut, false))
    {
        if (other.m_readyValue.has_value()) {
            // T 只保证 noexcept move construction，不能依赖 optional 的
            // move assignment。
            [[maybe_unused]] T& readyValue =
                m_readyValue.emplace(std::move(*other.m_readyValue));
            other.m_readyValue.reset();
        }
    }

    /**
     * @brief 在 await 协议开始前以移动构造语义替换 awaiter 状态。
     * @note await_ready() 或 await_suspend() 任一开始后不得再移动任一 awaiter。
     */
    UnboundedRecvAwaitable& operator=(UnboundedRecvAwaitable&& other) noexcept
    {
        if (this == &other) {
            return *this;
        }

        m_readyValue.reset();
        m_channel = std::exchange(other.m_channel, nullptr);
        m_timeoutTimer = std::move(other.m_timeoutTimer);
        if (other.m_readyValue.has_value()) {
            [[maybe_unused]] T& readyValue =
                m_readyValue.emplace(std::move(*other.m_readyValue));
            other.m_readyValue.reset();
        }
        m_waiterState = std::exchange(other.m_waiterState, nullptr);
        m_timedOut = std::exchange(other.m_timedOut, false);
        return *this;
    }

    bool await_ready() noexcept;

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) noexcept;

    /**
     * @brief 返回收到的消息。
     * @return 成功返回消息；关闭且排空返回 kClosed，超时或未取得消息返回 kTimeout。
     */
    std::expected<T, IOError> await_resume() noexcept;

private:
    friend class UnboundedChannel<T>;
    friend struct WithTimeout<UnboundedRecvAwaitable<T>>;

    /** @brief 绑定目标通道；等待完成前通道必须保持有效。 */
    explicit UnboundedRecvAwaitable(UnboundedChannel<T>* channel) noexcept
        : m_channel(channel) {}

    bool try_receive_now() noexcept;
    void mark_timeout() noexcept { m_timedOut = true; }
    void bind_timeout_timer(const TimeoutTimer::ptr& timer) noexcept
    {
        m_timeoutTimer = timer;
    }

    UnboundedChannel<T>* m_channel;
    TimeoutTimer::ptr m_timeoutTimer;
    std::optional<T> m_readyValue;
    TaskState* m_waiterState = nullptr;
    bool m_timedOut = false;
};

/**
 * @brief 最多接收指定数量消息的 vector-returning 批量异步等待体。
 * @details 为返回独立 vector，本等待体可能分配内存；OOM 敏感或要求数据搬运
 *          路径无分配时应使用 UnboundedRecvBatchToAwaitable。
 */
template <UnboundedValue T>
class UnboundedRecvBatchAwaitable
    : public TimeoutSupport<UnboundedRecvBatchAwaitable<T>>
{
public:
    UnboundedRecvBatchAwaitable(const UnboundedRecvBatchAwaitable&) = delete;
    UnboundedRecvBatchAwaitable& operator=(
        const UnboundedRecvBatchAwaitable&) = delete;

    /** @brief 在 await_ready()/await_suspend() 任一开始前转移等待体状态。 */
    UnboundedRecvBatchAwaitable(UnboundedRecvBatchAwaitable&& other) noexcept
        : m_channel(std::exchange(other.m_channel, nullptr))
        , m_timeoutTimer(std::move(other.m_timeoutTimer))
        , m_maxCount(std::exchange(other.m_maxCount, 0))
        , m_waiterState(std::exchange(other.m_waiterState, nullptr))
        , m_timedOut(std::exchange(other.m_timedOut, false))
    {
        if (other.m_readyValues.has_value()) {
            [[maybe_unused]] std::vector<T>& readyValues =
                m_readyValues.emplace(std::move(*other.m_readyValues));
            other.m_readyValues.reset();
        }
    }

    UnboundedRecvBatchAwaitable& operator=(
        UnboundedRecvBatchAwaitable&& other) noexcept
    {
        if (this == &other) {
            return *this;
        }

        m_readyValues.reset();
        m_channel = std::exchange(other.m_channel, nullptr);
        m_timeoutTimer = std::move(other.m_timeoutTimer);
        m_maxCount = std::exchange(other.m_maxCount, 0);
        if (other.m_readyValues.has_value()) {
            [[maybe_unused]] std::vector<T>& readyValues =
                m_readyValues.emplace(std::move(*other.m_readyValues));
            other.m_readyValues.reset();
        }
        m_waiterState = std::exchange(other.m_waiterState, nullptr);
        m_timedOut = std::exchange(other.m_timedOut, false);
        return *this;
    }

    bool await_ready();

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle);

    /**
     * @brief 返回收到的批次。
     * @return 成功返回批次；关闭且排空返回 kClosed，超时或未取得消息返回 kTimeout。
     * @note 构造返回 vector 可能分配内存，本接口不承诺 noexcept。
     */
    std::expected<std::vector<T>, IOError> await_resume();

private:
    friend class UnboundedChannel<T>;
    friend struct WithTimeout<UnboundedRecvBatchAwaitable<T>>;

    /**
     * @brief 绑定目标通道。
     * @param channel 目标通道；等待完成前必须保持有效。
     * @param maxCount 单次最多接收的消息数，0 表示立即返回空批次。
     */
    UnboundedRecvBatchAwaitable(UnboundedChannel<T>* channel,
                                size_t maxCount) noexcept
        : m_channel(channel), m_maxCount(maxCount) {}

    bool try_receive_now();
    void mark_timeout() noexcept { m_timedOut = true; }
    void bind_timeout_timer(const TimeoutTimer::ptr& timer) noexcept
    {
        m_timeoutTimer = timer;
    }

    UnboundedChannel<T>* m_channel;
    TimeoutTimer::ptr m_timeoutTimer;
    size_t m_maxCount;
    std::optional<std::vector<T>> m_readyValues;
    TaskState* m_waiterState = nullptr;
    bool m_timedOut = false;
};

/**
 * @brief 把批量消息追加到调用方预留 vector 的无分配异步等待体。
 * @details 等待期间只使用 destination 的现有 spare capacity，不扩容、不阻塞
 *          调度器线程；成功返回本次追加数量。
 * @note destination 必须活到 await 完成，且等待期间不得被其他线程或协程访问。
 */
template <UnboundedValue T>
class UnboundedRecvBatchToAwaitable
    : public TimeoutSupport<UnboundedRecvBatchToAwaitable<T>>
{
public:
    UnboundedRecvBatchToAwaitable(const UnboundedRecvBatchToAwaitable&) = delete;
    UnboundedRecvBatchToAwaitable& operator=(
        const UnboundedRecvBatchToAwaitable&) = delete;

    /** @brief 在 await_ready()/await_suspend() 任一开始前转移等待体状态。 */
    UnboundedRecvBatchToAwaitable(
        UnboundedRecvBatchToAwaitable&& other) noexcept
        : m_timeoutTimer(std::move(other.m_timeoutTimer))
        , m_channel(std::exchange(other.m_channel, nullptr))
        , m_destination(std::exchange(other.m_destination, nullptr))
        , m_waiterState(std::exchange(other.m_waiterState, nullptr))
        , m_maxCount(std::exchange(other.m_maxCount, 0))
        , m_readyCount(std::exchange(other.m_readyCount, 0))
        , m_ready(std::exchange(other.m_ready, false))
        , m_invalid(std::exchange(other.m_invalid, false))
        , m_timedOut(std::exchange(other.m_timedOut, false))
    {
    }

    UnboundedRecvBatchToAwaitable& operator=(
        UnboundedRecvBatchToAwaitable&& other) noexcept
    {
        if (this == &other) {
            return *this;
        }

        m_timeoutTimer = std::move(other.m_timeoutTimer);
        m_channel = std::exchange(other.m_channel, nullptr);
        m_destination = std::exchange(other.m_destination, nullptr);
        m_waiterState = std::exchange(other.m_waiterState, nullptr);
        m_maxCount = std::exchange(other.m_maxCount, 0);
        m_readyCount = std::exchange(other.m_readyCount, 0);
        m_ready = std::exchange(other.m_ready, false);
        m_invalid = std::exchange(other.m_invalid, false);
        m_timedOut = std::exchange(other.m_timedOut, false);
        return *this;
    }

    bool await_ready() noexcept;

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) noexcept;

    /**
     * @brief 返回追加数量。
     * @return maxCount==0 优先成功返回 0；否则 timeout 获胜返回 kTimeout，无
     *         spare capacity 返回 kParamInvalid，已发布尾消息优先排空并返回数量，
     *         关闭且排空返回 kClosed。
     */
    std::expected<size_t, IOError> await_resume() noexcept;

private:
    friend class UnboundedChannel<T>;

    /**
     * @brief 绑定目标通道和调用方缓冲区。
     * @param channel 目标通道；等待完成前必须保持有效。
     * @param destination 目标 vector；只使用现有 spare capacity。
     * @param maxCount 本次最多追加数量；0 表示立即成功并返回 0。
     */
    UnboundedRecvBatchToAwaitable(UnboundedChannel<T>* channel,
                                  std::vector<T>* destination,
                                  size_t maxCount) noexcept
        : m_channel(channel)
        , m_destination(destination)
        , m_maxCount(maxCount)
    {
    }
    friend struct WithTimeout<UnboundedRecvBatchToAwaitable<T>>;

    bool try_receive_now() noexcept;
    void mark_timeout() noexcept { m_timedOut = true; }
    void bind_timeout_timer(const TimeoutTimer::ptr& timer) noexcept
    {
        m_timeoutTimer = timer;
    }

    TimeoutTimer::ptr m_timeoutTimer;
    UnboundedChannel<T>* m_channel;
    std::vector<T>* m_destination;
    TaskState* m_waiterState = nullptr;
    size_t m_maxCount;
    size_t m_readyCount = 0;
    bool m_ready = false;
    bool m_invalid = false;
    bool m_timedOut = false;
};

/**
 * @brief 多生产者单消费者无界异步通道。
 * @tparam T 可移动元素类型。
 *
 * @details
 * - 每个显式 token 或默认线程生产者独占一条 SPSC 分块流。
 * - 同一生产者流内严格 FIFO；不同生产者之间不承诺全局顺序。
 * - 唯一消费者按固定配额轮询，避免持续活跃的生产者饿死稀疏生产者。
 * - send()/send_batch() 先完整预留所需分块，分配失败不会移动输入或部分发布。
 * - recv()/recv_batch() 空时只挂起协程，不阻塞调度器线程。
 * - 空转轮询影响吞吐时使用 drain_to() 批量排空；低频、延迟敏感或单次命中率高的
 *   路径可继续使用 try_recv()。
 *
 * @note 同一 ProducerToken 同时只能由一个生产者调用；所有接收操作必须串行。
 * @note T 的复制、移动构造和析构不得调用或销毁同一 channel，否则可能重入
 *       尚未提交的 producer/consumer cursor 或等待自身持有的 producer gate。
 * @note 销毁前必须由外部 join 所有 producer 调用栈，并等待全部接收 awaiter
 *       完成 await_resume()。token/TLS 缓存本身可以更晚析构，但 channel 销毁后
 *       不得再用于发送。
 */
template <UnboundedValue T>
class UnboundedChannel
{
private:
    static constexpr size_t kBlockTargetBytes = 4096;
    static constexpr size_t kConsumerQuota = 64;

    static void cpu_pause() noexcept
    {
#if defined(_MSC_VER)
        YieldProcessor();
#elif defined(__x86_64__) || defined(__i386__)
        _mm_pause();
#elif defined(__aarch64__) || defined(__arm__)
        __asm__ __volatile__("yield" ::: "memory");
#endif
    }

    struct Slot
    {
        alignas(T) std::byte storage[sizeof(T)];

        T* construction_address() noexcept
        {
            return reinterpret_cast<T*>(storage);
        }

        T* value() noexcept
        {
            return std::launder(reinterpret_cast<T*>(storage));
        }
    };

    static constexpr size_t kBlockCapacity =
        (kBlockTargetBytes - ::galay::utils::kCacheLineSize) /
                sizeof(Slot) < 2
        ? 2
        : (kBlockTargetBytes - ::galay::utils::kCacheLineSize) /
                sizeof(Slot);

    struct Block
    {
        alignas(::galay::utils::kCacheLineSize)
            std::atomic<Block*> next{nullptr};
        std::array<Slot, kBlockCapacity> slots;
    };

    enum class ProducerGate : uint8_t {
        kOpen,
        kSending,
        kPublished,
    };

    enum class ProducerLifetimeState : uint8_t {
        kOwned,
        kAvailable,
        kDetached,
    };

    /** @brief 可晚于 stream 析构释放的 producer 冷路径生命周期控制块。 */
    struct ProducerLifetime
    {
        ProducerLifetime() noexcept = default;
        ProducerLifetime(const ProducerLifetime&) = delete;
        ProducerLifetime& operator=(const ProducerLifetime&) = delete;
        ProducerLifetime(ProducerLifetime&&) = delete;
        ProducerLifetime& operator=(ProducerLifetime&&) = delete;

        std::atomic<uint32_t> references{2}; ///< stream anchor + 当前 owner。
        std::atomic<ProducerLifetimeState> state{
            ProducerLifetimeState::kOwned};
    };

    struct alignas(::galay::utils::kCacheLineSize) ProducerCursor
    {
        uint64_t localPublished = 0;
        Block* block = nullptr;
        size_t index = 0;
        bool activated = false;
    };

    struct alignas(::galay::utils::kCacheLineSize) ConsumerCursor
    {
        std::atomic<uint64_t> consumed{0};
        uint64_t localConsumed = 0;
        uint64_t observedPublished = 0;
        Block* block = nullptr;
        size_t index = 0;
    };

    struct ProducerStream;

    struct alignas(::galay::utils::kCacheLineSize) StreamSharedState
    {
        std::atomic<uint64_t> published{0};
        std::atomic<Block*> recycledBlocks{nullptr};
        ProducerStream* readyNext = nullptr;
        std::atomic<bool> active{false};
    };

    struct alignas(::galay::utils::kCacheLineSize) ProducerControl
    {
        std::atomic<ProducerGate> gate{ProducerGate::kOpen};
    };

    /**
     * @brief 批量构造未完成时销毁已构造元素并恢复 producer cursor。
     * @note producer cursor 始终位于当前流链尾，当前位置之后没有已发布数据，
     *       因此回滚时可安全释放本次预留的后继 block chain。
     */
    struct BatchConstructionGuard
    {
        explicit BatchConstructionGuard(ProducerCursor& cursor) noexcept
            : producer(cursor), initialBlock(cursor.block), initialIndex(cursor.index) {}

        BatchConstructionGuard(const BatchConstructionGuard&) = delete;
        BatchConstructionGuard& operator=(const BatchConstructionGuard&) = delete;
        BatchConstructionGuard(BatchConstructionGuard&&) = delete;
        BatchConstructionGuard& operator=(BatchConstructionGuard&&) = delete;

        ~BatchConstructionGuard() noexcept
        {
            if (committed) {
                return;
            }

            Block* block = initialBlock;
            size_t index = initialIndex;
            for (size_t count = 0; count < constructed; ++count) {
                if (index == kBlockCapacity) {
                    block = block->next.load(std::memory_order_relaxed);
                    index = 0;
                }
                std::destroy_at(block->slots[index].value());
                ++index;
            }
            producer.block = initialBlock;
            producer.index = initialIndex;

            Block* unused = initialBlock->next.load(std::memory_order_relaxed);
            initialBlock->next.store(nullptr, std::memory_order_relaxed);
            destroy_block_chain(unused);
        }

        void record_constructed() noexcept
        {
            ++constructed;
        }

        void commit() noexcept
        {
            committed = true;
        }

        ProducerCursor& producer;
        Block* const initialBlock;
        const size_t initialIndex;
        size_t constructed = 0;
        bool committed = false;
    };

    /** @brief 一个生产者独占写入、唯一消费者独占读取的分块流。 */
    struct alignas(::galay::utils::kCacheLineSize) ProducerStream
    {
        ProducerStream(Block* first, ProducerLifetime* streamLifetime) noexcept
            : lifetime(streamLifetime)
        {
            producer.block = first;
            consumer.block = first;
        }

        ProducerCursor producer;
        ConsumerCursor consumer;
        StreamSharedState shared;
        ProducerControl control;
        ProducerLifetime* lifetime = nullptr;
        ProducerStream* next = nullptr;
    };

    enum class WaiterPhase : uint8_t {
        kIdle,
        kArming,
        kArmingPending,
        kArmed,
        kWaking,
    };

    enum class CloseState : uint8_t {
        kOpen,
        kClosing,
        kClosed,
    };

    struct DefaultProducerCacheEntry
    {
        const UnboundedChannel* channel = nullptr;
        ProducerStream* stream = nullptr;
        ProducerLifetime* lifetime = nullptr;
        DefaultProducerCacheEntry* next = nullptr;
        uint64_t generation = 0;
    };

    static void release_producer_lifetime_reference(
        ProducerLifetime* lifetime) noexcept
    {
        const uint32_t previous =
            lifetime->references.fetch_sub(1, std::memory_order_acq_rel);
        if (previous == 1) {
            delete lifetime;
        }
    }

    static void retain_producer_lifetime(ProducerLifetime* lifetime) noexcept
    {
        // stream anchor 保证旧引用至少为 1；这里只需要增加 owner 引用。
        [[maybe_unused]] const uint32_t previous =
            lifetime->references.fetch_add(1, std::memory_order_relaxed);
    }

    static void release_producer_owner(ProducerLifetime* lifetime) noexcept
    {
        if (lifetime == nullptr) {
            return;
        }
        ProducerLifetimeState state =
            lifetime->state.load(std::memory_order_acquire);
        while (state == ProducerLifetimeState::kOwned &&
               !lifetime->state.compare_exchange_weak(
                   state,
                   ProducerLifetimeState::kAvailable,
                   std::memory_order_release,
                   std::memory_order_acquire)) {
        }
        release_producer_lifetime_reference(lifetime);
    }

    /**
     * @brief 默认 send() 的线程本地流缓存。
     * @note 析构只释放独立生命周期控制块和缓存节点，绝不访问 channel 或
     *       ProducerStream，因此允许晚于 channel 析构。
     */
    struct DefaultProducerCache
    {
        ~DefaultProducerCache() noexcept
        {
            while (head != nullptr) {
                DefaultProducerCacheEntry* next = head->next;
                release_producer_owner(head->lifetime);
                delete head;
                head = next;
            }
        }

        DefaultProducerCacheEntry* head = nullptr;
    };

public:
    static constexpr size_t DEFAULT_BATCH_SIZE = 1024;

    /**
     * @brief 生产者独占句柄。
     * @details token 直接绑定一条 ProducerStream，绕过默认 TLS 查找；可移动但不可复制。
     * @note token 移动或销毁不会删除流，channel 仍可排空流中已经发布的消息，
     *       并可把该流交给后续 producer 继续追加。
     * @note moved-from token、其他 channel 的 token 和 channel 析构后 detach 的
     *       token 均不可用于发送。
     */
    class ProducerToken
    {
    public:
        ProducerToken(const ProducerToken&) = delete;
        ProducerToken& operator=(const ProducerToken&) = delete;

        ProducerToken(ProducerToken&& other) noexcept
            : m_channel(std::exchange(other.m_channel, nullptr))
            , m_stream(std::exchange(other.m_stream, nullptr))
            , m_lifetime(std::exchange(other.m_lifetime, nullptr))
            , m_generation(std::exchange(other.m_generation, 0))
        {
        }

        ProducerToken& operator=(ProducerToken&& other) noexcept
        {
            if (this != &other) {
                reset();
                m_channel = std::exchange(other.m_channel, nullptr);
                m_stream = std::exchange(other.m_stream, nullptr);
                m_lifetime = std::exchange(other.m_lifetime, nullptr);
                m_generation = std::exchange(other.m_generation, 0);
            }
            return *this;
        }

        ~ProducerToken() noexcept
        {
            reset();
        }

        /**
         * @brief 返回 token 是否仍绑定一个存活 channel 的生产者流。
         * @note close() 不使 token 失效，但后续 send() 会返回 false；channel 析构
         *       并 detach stream 后返回 false。
         */
        [[nodiscard]] bool valid() const noexcept
        {
            ProducerLifetime* lifetime = m_lifetime;
            if (lifetime == nullptr ||
                lifetime->state.load(std::memory_order_acquire) !=
                    ProducerLifetimeState::kOwned) {
                return false;
            }
            return m_channel != nullptr && m_stream != nullptr &&
                m_generation != 0;
        }

    private:
        friend class UnboundedChannel;

        ProducerToken() noexcept = default;

        ProducerToken(UnboundedChannel* channel,
                      ProducerStream* stream,
                      ProducerLifetime* lifetime,
                      uint64_t generation) noexcept
            : m_channel(channel)
            , m_stream(stream)
            , m_lifetime(lifetime)
            , m_generation(generation)
        {
        }

        void reset() noexcept
        {
            ProducerLifetime* lifetime =
                std::exchange(m_lifetime, nullptr);
            m_channel = nullptr;
            m_stream = nullptr;
            m_generation = 0;
            release_producer_owner(lifetime);
        }

        [[nodiscard]] bool valid_for(const UnboundedChannel* channel) const noexcept
        {
            ProducerLifetime* lifetime = m_lifetime;
            if (lifetime == nullptr ||
                lifetime->state.load(std::memory_order_acquire) !=
                    ProducerLifetimeState::kOwned) {
                return false;
            }
            return m_channel != nullptr && m_stream != nullptr &&
                m_generation != 0 && m_channel == channel &&
                m_generation == channel->m_generation;
        }

        /**
         * @brief 校验一个存活 channel 的同步发送热路径所需的 token 关联。
         * @pre channel 仍存活，且同一 token 未被其他线程并发移动或销毁。
         * @note channel 的 send() 调用和 ProducerToken 的单生产者契约已满足此前提；
         *       不读取 lifetime 状态，避免每条消息为析构后 valid() 查询付出 acquire。
         */
        [[nodiscard]] bool valid_for_live_channel(
            const UnboundedChannel* channel) const noexcept
        {
            return m_lifetime != nullptr && m_channel == channel &&
                m_stream != nullptr && m_generation != 0 &&
                m_generation == channel->m_generation;
        }

        UnboundedChannel* m_channel = nullptr;
        ProducerStream* m_stream = nullptr;
        ProducerLifetime* m_lifetime = nullptr;
        uint64_t m_generation = 0;
    };

    /**
     * @brief 构造 MPSC 通道。
     * @param defaultBatchSize recv_batch() 未指定上限时的默认值。
     * @param singleRecvPrefetchLimit 单条接收后最多预取的消息数；默认不预取。
     *        b26 的新 ready 数据面测量表明同步单条接收直接轮转 stream 更快；
     *        显式预取仍只在当前一个活跃 producer stream 时启用。
     */
    explicit UnboundedChannel(size_t defaultBatchSize = DEFAULT_BATCH_SIZE,
                              size_t singleRecvPrefetchLimit = 0)
        : m_generation(next_generation())
        , m_defaultBatchSize(std::max<size_t>(1, defaultBatchSize))
        , m_singleRecvPrefetchLimit(singleRecvPrefetchLimit)
    {
        m_singleRecvPrefetch.reserve(singleRecvPrefetchLimit);
    }

    /**
     * @brief 销毁预取值、所有未消费消息、流和分块。
     * @pre 已由外部 join 所有 producer 调用栈，全部接收 awaiter 已完成
     *      await_resume()，且不得再有并发调用、挂起 waiter 或仍会被用于发送的
     *      ProducerToken。token/TLS 缓存本身可以更晚析构。
     */
    ~UnboundedChannel() noexcept
    {
        ProducerStream* stream = m_streamHead.load(std::memory_order_relaxed);
        while (stream != nullptr) {
            ProducerStream* next = stream->next;
            destroy_stream(stream);
            stream = next;
        }
    }

    UnboundedChannel(const UnboundedChannel&) = delete;
    UnboundedChannel& operator=(const UnboundedChannel&) = delete;
    UnboundedChannel(UnboundedChannel&&) = delete;
    UnboundedChannel& operator=(UnboundedChannel&&) = delete;

    /**
     * @brief 创建生产者独占 token。
     * @return 创建成功时 valid()==true；通道已开始关闭或内存不足时返回无效 token。
     */
    [[nodiscard]] ProducerToken make_producer_token() noexcept
    {
        ProducerStream* stream = acquire_producer_stream();
        if (stream == nullptr) {
            return ProducerToken();
        }
        return ProducerToken(this, stream, stream->lifetime, m_generation);
    }

    /**
     * @brief 使用当前线程的默认生产者流发送一条消息。
     * @return 发布成功返回 true；通道已开始关闭或流/分块分配失败返回 false，
     *         value 保持未移动。
     */
    [[nodiscard]] bool send(T&& value) noexcept
    {
        ProducerStream* stream = default_producer_stream();
        return stream != nullptr && send_to_stream(*stream, std::move(value));
    }

    /**
     * @brief 使用显式 token 发送一条消息。
     * @return 发布成功返回 true；token 无效、属于其他 channel、通道已开始关闭或
     *         分块分配失败返回 false，value 保持未移动。
     */
    [[nodiscard]] bool send(ProducerToken& token, T&& value) noexcept
    {
        return token.valid_for_live_channel(this) &&
            send_to_stream(*token.m_stream, std::move(value));
    }

    /**
     * @brief 复制并使用默认生产者流发送一条消息。
     * @return 发布成功返回 true；通道已开始关闭或流/分块分配失败返回 false。
     */
    [[nodiscard]] bool send(const T& value)
        noexcept requires std::is_nothrow_copy_constructible_v<T>
    {
        T copy = value;
        return send(std::move(copy));
    }

    /**
     * @brief 复制并使用显式 token 发送一条消息。
     * @return 发布成功返回 true；token 无效/foreign、通道已开始关闭或分块分配
     *         失败返回 false。
     */
    [[nodiscard]] bool send(ProducerToken& token, const T& value)
        noexcept requires std::is_nothrow_copy_constructible_v<T>
    {
        if (!token.valid_for_live_channel(this)) {
            return false;
        }
        T copy = value;
        return send(token, std::move(copy));
    }

    /**
     * @brief 复制并原子发布一批消息到默认生产者流。
     * @return 全批发布返回 true；通道已开始关闭或预留失败返回 false，通道保持不变。
     */
    [[nodiscard]] bool send_batch(const std::vector<T>& values)
        noexcept requires std::is_nothrow_copy_constructible_v<T>
    {
        if (values.empty()) {
            return m_closeState.load(std::memory_order_seq_cst) ==
                CloseState::kOpen;
        }
        ProducerStream* stream = default_producer_stream();
        return stream != nullptr && send_batch_to_stream(*stream, values);
    }

    /**
     * @brief 复制并原子发布一批消息到显式 token 的流。
     * @return 全批发布返回 true；token 无效/foreign、通道已开始关闭或预留失败
     *         返回 false，通道保持不变。
     */
    [[nodiscard]] bool send_batch(ProducerToken& token,
                                 const std::vector<T>& values)
        noexcept requires std::is_nothrow_copy_constructible_v<T>
    {
        if (!token.valid_for_live_channel(this)) {
            return false;
        }
        if (values.empty()) {
            return m_closeState.load(std::memory_order_seq_cst) ==
                CloseState::kOpen;
        }
        return send_batch_to_stream(*token.m_stream, values);
    }

    /**
     * @brief 移动并原子发布一批消息到默认生产者流。
     * @return 全批发布返回 true；通道已开始关闭或预留失败返回 false，values
     *         保持未移动。
     */
    [[nodiscard]] bool send_batch(std::vector<T>&& values) noexcept
    {
        if (values.empty()) {
            return m_closeState.load(std::memory_order_seq_cst) ==
                CloseState::kOpen;
        }
        ProducerStream* stream = default_producer_stream();
        return stream != nullptr && send_batch_to_stream(*stream, std::move(values));
    }

    /**
     * @brief 移动并原子发布一批消息到显式 token 的流。
     * @return 全批发布返回 true；token 无效/foreign、通道已开始关闭或预留失败
     *         返回 false，values 保持未移动。
     */
    [[nodiscard]] bool send_batch(
        ProducerToken& token, std::vector<T>&& values) noexcept
    {
        if (!token.valid_for_live_channel(this)) {
            return false;
        }
        if (values.empty()) {
            return m_closeState.load(std::memory_order_seq_cst) ==
                CloseState::kOpen;
        }
        return send_batch_to_stream(*token.m_stream, std::move(values));
    }

    /** @brief 返回单条异步接收等待体。 */
    [[nodiscard]] UnboundedRecvAwaitable<T> recv() noexcept
    {
        return UnboundedRecvAwaitable<T>(this);
    }

    /**
     * @brief 返回使用默认批量上限的 vector-returning 异步接收等待体。
     * @note 接收路径可能分配；OOM 敏感或要求无分配时使用 recv_batch_to()。
     */
    [[nodiscard]] UnboundedRecvBatchAwaitable<T> recv_batch() noexcept
    {
        return UnboundedRecvBatchAwaitable<T>(this, m_defaultBatchSize);
    }

    /**
     * @brief 返回最多接收 maxCount 条消息的 vector-returning 异步等待体。
     * @note 接收路径可能分配；OOM 敏感或要求无分配时使用 recv_batch_to()。
     */
    [[nodiscard]] UnboundedRecvBatchAwaitable<T> recv_batch(size_t maxCount) noexcept
    {
        return UnboundedRecvBatchAwaitable<T>(this, maxCount);
    }

    /**
     * @brief 返回使用默认批量上限、追加到调用方预留 vector 的异步等待体。
     * @param destination 目标 vector；只使用现有 spare capacity。
     * @return 数据搬运路径不分配的等待体；无 spare capacity 时以
     *         kParamInvalid 完成。timeout() wrapper 自身可能分配 timer 状态。
     */
    [[nodiscard]] UnboundedRecvBatchToAwaitable<T> recv_batch_to(
        std::vector<T>& destination) noexcept
    {
        return UnboundedRecvBatchToAwaitable<T>(
            this, &destination, m_defaultBatchSize);
    }

    /**
     * @brief 返回最多追加 maxCount 条消息到调用方预留 vector 的异步等待体。
     * @param destination 目标 vector；只使用现有 spare capacity。
     * @param maxCount 本次最多追加数量；0 表示优先立即成功并返回 0。大于
     *        spare capacity 时填满 spare 后成功返回实际数量。
     * @return 数据搬运路径不分配的等待体；maxCount>0 且无 spare capacity
     *         时以 kParamInvalid 完成。timeout() wrapper 自身可能分配 timer 状态。
     */
    [[nodiscard]] UnboundedRecvBatchToAwaitable<T> recv_batch_to(
        std::vector<T>& destination, size_t maxCount) noexcept
    {
        return UnboundedRecvBatchToAwaitable<T>(this, &destination, maxCount);
    }

    /**
     * @brief 非阻塞接收一条消息。
     * @return 成功返回消息；所有流当前均为空时返回 std::nullopt。
     * @note 仅唯一消费者可调用；本函数不执行 cursor CAS 或 RMW。
     */
    [[nodiscard]] std::optional<T> try_recv() noexcept
    {
        if (auto cached = try_pop_prefetched_value(); cached.has_value()) {
            return cached;
        }
        auto value = try_pop_next_value();
        if (value.has_value()) {
            try_prefetch_single_recv_values();
        }
        return value;
    }

    /**
     * @brief 把消息追加到调用方预留的 vector，且绝不扩容。
     * @param destination 目标 vector；只使用现有 spare capacity。
     * @param maxCount 本次最多排空的消息数。
     * @return 实际追加数量；容量不足或当前为空时返回 0。
     * @note 仅唯一消费者可调用，稳态路径不分配内存。
     */
    [[nodiscard]] size_t drain_to(
        std::vector<T>& destination, size_t maxCount) noexcept
    {
        const size_t spare = destination.capacity() - destination.size();
        const size_t limit = std::min(maxCount, spare);
        const size_t initialSize = destination.size();
        while (destination.size() - initialSize < limit) {
            auto value = try_recv();
            if (!value.has_value()) {
                break;
            }
            destination.push_back(std::move(*value));
        }
        return destination.size() - initialSize;
    }

    /**
     * @brief 使用构造时配置的默认上限非阻塞批量接收。
     * @note 返回 vector 可能分配；OOM 敏感路径使用 drain_to()。
     */
    [[nodiscard]] std::optional<std::vector<T>> try_recv_batch()
    {
        return try_recv_batch(m_defaultBatchSize);
    }

    /**
     * @brief 非阻塞批量接收。
     * @param maxCount 单次最多接收数量，0 返回空 vector 且不消费消息。
     * @return 收到至少一条时返回批次；当前为空返回 std::nullopt。
     * @note 返回 vector 可能分配；OOM 敏感路径使用 drain_to()。
     */
    [[nodiscard]] std::optional<std::vector<T>> try_recv_batch(size_t maxCount)
    {
        if (maxCount == 0) {
            return std::vector<T>{};
        }

        const size_t available = size();
        if (available == 0) {
            return std::nullopt;
        }

        std::vector<T> values;
        const size_t limit = std::min({maxCount, available, values.max_size()});
        values.reserve(limit);
        while (values.size() < limit) {
            auto cached = try_pop_prefetched_value();
            if (!cached.has_value()) {
                break;
            }
            values.push_back(std::move(*cached));
        }
        while (values.size() < limit) {
            auto value = try_pop_next_value();
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
     * @brief 返回所有生产者流和预取缓存中待消费消息数的近似快照。
     * @note 该诊断操作按 producer 数量线性扫描，不参与收发同步。
     */
    [[nodiscard]] size_t size() const noexcept
    {
        size_t total = m_prefetchedVisible.load(std::memory_order_acquire);
        constexpr size_t kMaxSize = std::numeric_limits<size_t>::max();
        constexpr uint64_t kMaxPlausiblePending =
            static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
        ProducerStream* stream = m_streamHead.load(std::memory_order_acquire);
        while (stream != nullptr) {
            const uint64_t published =
                stream->shared.published.load(std::memory_order_acquire);
            const uint64_t consumed =
                stream->consumer.consumed.load(std::memory_order_acquire);
            const uint64_t pending = published - consumed;
            // 累计序号按 uint64_t 模回绕；单流不可能在进程地址空间中积压
            // 2^63 条消息。超过半区间表示本次跨原子快照读到了较新的 consumed
            // 和较旧的 published，诊断值宁可暂时少算也不能误报为巨量积压。
            if (pending > kMaxPlausiblePending) {
                stream = stream->next;
                continue;
            }
            const uint64_t room = static_cast<uint64_t>(kMaxSize - total);
            if (pending > room) {
                return kMaxSize;
            }
            total += static_cast<size_t>(pending);
            stream = stream->next;
        }
        return total;
    }

    /** @brief 近似检查当前是否没有待消费消息。 */
    [[nodiscard]] bool empty() const noexcept
    {
        return size() == 0;
    }

    /**
     * @brief 关闭通道并唤醒挂起的接收者。
     * @details 首个调用以 Open->Closing 建立发送 admission cutoff，再等待 cutoff
     *          前已进入的 producer registration 和所有已经取得许可的发送完成；
     *          最终 Closed 发布是唯一永久关闭位和完成屏障，保证这些发送发布的
     *          尾消息在 close() 返回前可见。
     * @return 本次调用取得关闭权并完成关闭返回 true；其他 close 已取得关闭权
     *         （包括仍处于 Closing）时立即返回 false。
     * @note cutoff 前已取得许可的发送可能成功；cutoff 后的新发送必定失败。
     * @note 本函数会等待 cutoff 前已经开始注册或取得发送许可的 producer 完成，
     *       不应在这些操作可能长期停顿时从事件循环线程调用。
     * @note close() 只建立 channel 数据发布与访问完成屏障；它不 join producer
     *       调用栈，也不等待已唤醒 receiver 执行 await_resume()。销毁前仍须满足
     *       析构函数的外部 quiescence 前置条件。
     */
    [[nodiscard]] bool close() noexcept
    {
        CloseState expected = CloseState::kOpen;
        if (!m_closeState.compare_exchange_strong(
                expected,
                CloseState::kClosing,
                std::memory_order_seq_cst,
                std::memory_order_seq_cst)) {
            return false;
        }

        while (m_producerRegistrations.load(std::memory_order_seq_cst) != 0) {
            std::this_thread::yield();
        }
        ProducerStream* stream =
            m_streamHead.load(std::memory_order_acquire);
        while (stream != nullptr) {
            size_t spins = 0;
            while (stream->control.gate.load(std::memory_order_seq_cst) !=
                   ProducerGate::kOpen) {
                if (spins < 64) {
                    cpu_pause();
                    ++spins;
                } else {
                    std::this_thread::yield();
                    spins = 0;
                }
            }
            stream = stream->next;
        }
        m_closeState.store(CloseState::kClosed,
                           std::memory_order_seq_cst);
        TaskState* waiterState = detach_published_waiter();
        // close 的全部 channel 访问在调度前完成，允许 waiter 内联重入 close()。
        if (waiterState != nullptr) {
            wake_detached_waiter(waiterState);
        }
        return true;
    }

    /** @brief 返回通道是否已经进入 Closing 或 Closed。 */
    [[nodiscard]] bool is_closed() const noexcept
    {
        return m_closeState.load(std::memory_order_acquire) != CloseState::kOpen;
    }

    /** @brief 返回通道是否已关闭且所有已发布消息均已排空。 */
    [[nodiscard]] bool is_closed_and_drained() const noexcept
    {
        return m_closeState.load(std::memory_order_seq_cst) ==
                CloseState::kClosed &&
            empty();
    }

private:
    template <UnboundedValue U>
    friend class UnboundedRecvAwaitable;
    template <UnboundedValue U>
    friend class UnboundedRecvBatchAwaitable;
    template <UnboundedValue U>
    friend class UnboundedRecvBatchToAwaitable;
    friend struct UnboundedChannelTestAccess;

    inline static std::atomic<uint64_t> s_generationSeed{1};

    static uint64_t next_generation() noexcept
    {
        uint64_t generation =
            s_generationSeed.fetch_add(1, std::memory_order_relaxed);
        if (generation == 0) {
            generation = s_generationSeed.fetch_add(1, std::memory_order_relaxed);
        }
        return generation;
    }

    static DefaultProducerCache& default_producer_cache() noexcept
    {
        static thread_local DefaultProducerCache cache;
        return cache;
    }

    static void destroy_block_chain(Block* block) noexcept
    {
        while (block != nullptr) {
            Block* next = block->next.load(std::memory_order_relaxed);
            delete block;
            block = next;
        }
    }

    static void destroy_stream(ProducerStream* stream) noexcept
    {
        ProducerLifetime* lifetime = stream->lifetime;
        lifetime->state.store(ProducerLifetimeState::kDetached,
                              std::memory_order_release);
        ConsumerCursor& consumer = stream->consumer;
        const uint64_t published =
            stream->shared.published.load(std::memory_order_relaxed);
        while (consumer.localConsumed != published && consumer.block != nullptr) {
            if (consumer.index == kBlockCapacity) {
                Block* retired = consumer.block;
                consumer.block = retired->next.load(std::memory_order_relaxed);
                consumer.index = 0;
                delete retired;
                continue;
            }
            std::destroy_at(consumer.block->slots[consumer.index].value());
            ++consumer.index;
            ++consumer.localConsumed;
        }
        destroy_block_chain(consumer.block);
        destroy_block_chain(
            stream->shared.recycledBlocks.load(std::memory_order_relaxed));
        delete stream;
        release_producer_lifetime_reference(lifetime);
    }

    ProducerStream* acquire_producer_stream() noexcept
    {
        // fetch_add/sub 的返回值只是旧计数快照；关闭仲裁只依赖原子副作用。
        [[maybe_unused]] const size_t registrationsBeforeAcquire =
            m_producerRegistrations.fetch_add(1, std::memory_order_seq_cst);
        if (m_closeState.load(std::memory_order_seq_cst) != CloseState::kOpen) {
            [[maybe_unused]] const size_t registrationsBeforeRelease =
                m_producerRegistrations.fetch_sub(1, std::memory_order_seq_cst);
            return nullptr;
        }

        ProducerStream* reusable =
            m_streamHead.load(std::memory_order_acquire);
        while (reusable != nullptr) {
            ProducerLifetimeState expected =
                ProducerLifetimeState::kAvailable;
            if (reusable->lifetime->state.compare_exchange_strong(
                    expected,
                    ProducerLifetimeState::kOwned,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                retain_producer_lifetime(reusable->lifetime);
                [[maybe_unused]] const size_t registrationsBeforeRelease =
                    m_producerRegistrations.fetch_sub(
                        1, std::memory_order_seq_cst);
                return reusable;
            }
            reusable = reusable->next;
        }

        Block* first = new (std::nothrow) Block;
        if (first == nullptr) {
            [[maybe_unused]] const size_t registrationsBeforeRelease =
                m_producerRegistrations.fetch_sub(1, std::memory_order_seq_cst);
            return nullptr;
        }
        ProducerLifetime* lifetime = new (std::nothrow) ProducerLifetime;
        if (lifetime == nullptr) {
            delete first;
            [[maybe_unused]] const size_t registrationsBeforeRelease =
                m_producerRegistrations.fetch_sub(1, std::memory_order_seq_cst);
            return nullptr;
        }
        ProducerStream* stream =
            new (std::nothrow) ProducerStream(first, lifetime);
        if (stream == nullptr) {
            delete lifetime;
            delete first;
            [[maybe_unused]] const size_t registrationsBeforeRelease =
                m_producerRegistrations.fetch_sub(1, std::memory_order_seq_cst);
            return nullptr;
        }

        ProducerStream* head = m_streamHead.load(std::memory_order_relaxed);
        do {
            stream->next = head;
        } while (!m_streamHead.compare_exchange_weak(
            head,
            stream,
            std::memory_order_release,
            std::memory_order_relaxed));
        [[maybe_unused]] const size_t registrationsBeforeRelease =
            m_producerRegistrations.fetch_sub(1, std::memory_order_seq_cst);
        return stream;
    }

    static Block* take_recycled_block(ProducerStream& stream) noexcept
    {
        Block* block =
            stream.shared.recycledBlocks.load(std::memory_order_acquire);
        while (block != nullptr) {
            Block* next = block->next.load(std::memory_order_relaxed);
            if (stream.shared.recycledBlocks.compare_exchange_weak(
                    block,
                    next,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                block->next.store(nullptr, std::memory_order_relaxed);
                return block;
            }
        }
        return new (std::nothrow) Block;
    }

    static void recycle_block(ProducerStream& stream, Block* block) noexcept
    {
        Block* recycled =
            stream.shared.recycledBlocks.load(std::memory_order_relaxed);
        do {
            block->next.store(recycled, std::memory_order_relaxed);
        } while (!stream.shared.recycledBlocks.compare_exchange_weak(
            recycled,
            block,
            std::memory_order_release,
            std::memory_order_relaxed));
    }

    ProducerStream* default_producer_stream() noexcept
    {
        DefaultProducerCache& cache = default_producer_cache();
        DefaultProducerCacheEntry** link = &cache.head;
        while (*link != nullptr) {
            DefaultProducerCacheEntry* entry = *link;
            if (entry->lifetime->state.load(std::memory_order_acquire) ==
                ProducerLifetimeState::kDetached) {
                *link = entry->next;
                release_producer_owner(entry->lifetime);
                delete entry;
                continue;
            }
            if (entry->channel == this && entry->generation == m_generation) {
                if (link != &cache.head) {
                    *link = entry->next;
                    entry->next = cache.head;
                    cache.head = entry;
                }
                return entry->stream;
            }
            link = &entry->next;
        }

        auto* entry = new (std::nothrow) DefaultProducerCacheEntry;
        if (entry == nullptr) {
            return nullptr;
        }
        ProducerStream* stream = acquire_producer_stream();
        if (stream == nullptr) {
            delete entry;
            return nullptr;
        }
        entry->channel = this;
        entry->stream = stream;
        entry->lifetime = stream->lifetime;
        entry->generation = m_generation;
        entry->next = cache.head;
        cache.head = entry;
        return stream;
    }

    static bool reserve_producer_slots(ProducerStream& stream, size_t count) noexcept
    {
        if (count == 0) {
            return true;
        }
        ProducerCursor& producer = stream.producer;
        const size_t remaining = kBlockCapacity - producer.index;
        if (count <= remaining) {
            return true;
        }

        const size_t missing = count - remaining;
        const size_t blocksNeeded =
            (missing + kBlockCapacity - 1) / kBlockCapacity;
        Block* first = nullptr;
        Block* last = nullptr;
        for (size_t i = 0; i < blocksNeeded; ++i) {
            Block* block = take_recycled_block(stream);
            if (block == nullptr) {
                destroy_block_chain(first);
                return false;
            }
            if (last == nullptr) {
                first = block;
            } else {
                last->next.store(block, std::memory_order_relaxed);
            }
            last = block;
        }
        Block* const previousTail = producer.block;
        if (remaining == 0) {
            // consumer 一旦取得 next 就会回收 previousTail，并复用它的 next 作为
            // recycle 链接；因此满块时必须先移动 producer 私有 cursor，再发布
            // previousTail->next，不能在发布后重新读取这个共享链接。
            producer.block = first;
            producer.index = 0;
        }
        previousTail->next.store(first, std::memory_order_release);
        return true;
    }

    void push_ready_stream(ProducerStream& stream) noexcept
    {
        ProducerStream* ready = m_readyStack.load(std::memory_order_relaxed);
        do {
            stream.shared.readyNext = ready;
        } while (!m_readyStack.compare_exchange_weak(
            ready,
            &stream,
            std::memory_order_seq_cst,
            std::memory_order_relaxed));
    }

    void activate_ready_stream(ProducerStream& stream) noexcept
    {
        if (stream.shared.active.load(std::memory_order_relaxed)) {
            return;
        }
        bool expected = false;
        if (stream.shared.active.compare_exchange_strong(
                expected,
                true,
                std::memory_order_acq_rel,
                std::memory_order_relaxed)) {
            push_ready_stream(stream);
        }
    }

    [[nodiscard]] TaskState* publish_stream(ProducerStream& stream,
                                           size_t count) noexcept
    {
        ProducerCursor& producer = stream.producer;
        producer.localPublished += static_cast<uint64_t>(count);
        if (!producer.activated) {
            activate_ready_stream(stream);
            producer.activated = true;
        }
        // 累计 tail 是整批的唯一发布点；consumer 的 acquire 同步本批
        // 全部元素构造和 block 链接，不会观察到部分批次。
        stream.shared.published.store(producer.localPublished,
                                      std::memory_order_release);
        // 消息数据和 stream membership 都先于 SC 通知状态可见，使 waiter arming
        // 能区分仍在构造的发送与已经可消费的数据。
        stream.control.gate.store(
            ProducerGate::kPublished,
            std::memory_order_seq_cst);
        return detach_published_waiter();
    }

    /**
     * @brief 在当前 producer stream 上取得发送许可。
     * @return close 线性化前取得许可返回 true；否则恢复 open 并返回 false。
     * @note 同一 stream 只能由一个 producer 串行调用。producer 先以 seq_cst
     *       store 宣告 Sending，再读取 close state；close 以相反顺序发布 cutoff
     *       并读取 gate。该非对称握手共享同一全序，保证双方不可能同时漏看，且
     *       steady-state send 不执行 RMW。
     */
    [[nodiscard]] bool begin_send(ProducerStream& stream) noexcept
    {
        if (m_closeState.load(std::memory_order_relaxed) != CloseState::kOpen) {
            return false;
        }

        stream.control.gate.store(
            ProducerGate::kSending,
            std::memory_order_seq_cst);
        if (m_closeState.load(std::memory_order_seq_cst) == CloseState::kOpen) {
            return true;
        }
        stream.control.gate.store(ProducerGate::kOpen,
                                  std::memory_order_release);
        return false;
    }

    static void finish_send(ProducerStream& stream) noexcept
    {
        stream.control.gate.store(ProducerGate::kOpen,
                                  std::memory_order_release);
    }

    bool send_to_stream(ProducerStream& stream, T&& value) noexcept
    {
        if (!begin_send(stream)) {
            return false;
        }
        if (!reserve_producer_slots(stream, 1)) {
            finish_send(stream);
            return false;
        }
        ProducerCursor& producer = stream.producer;
        [[maybe_unused]] T* const stored = std::construct_at(
            producer.block->slots[producer.index].construction_address(),
            std::move(value));
        ++producer.index;
        TaskState* waiterState = publish_stream(stream, 1);
        // finish_send() 是本 sender 对 channel/stream 的最后一次访问。只有先
        // 释放 gate，内联恢复的 receiver 才能安全重入 close()。
        finish_send(stream);
        if (waiterState != nullptr) {
            wake_detached_waiter(waiterState);
        }
        return true;
    }

    bool send_batch_to_stream(ProducerStream& stream, const std::vector<T>& values)
        noexcept requires std::is_nothrow_copy_constructible_v<T>
    {
        if (!begin_send(stream)) {
            return false;
        }
        if (!reserve_producer_slots(stream, values.size())) {
            finish_send(stream);
            return false;
        }
        ProducerCursor& producer = stream.producer;
        BatchConstructionGuard guard(producer);
        for (const T& value : values) {
            if (producer.index == kBlockCapacity) {
                producer.block = producer.block->next.load(std::memory_order_acquire);
                producer.index = 0;
            }
            [[maybe_unused]] T* const stored = std::construct_at(
                producer.block->slots[producer.index].construction_address(), value);
            ++producer.index;
            guard.record_constructed();
        }
        guard.commit();
        TaskState* waiterState = publish_stream(stream, values.size());
        // waiter 已完全摘出，释放 gate 后的调度不再访问 channel/stream。
        finish_send(stream);
        if (waiterState != nullptr) {
            wake_detached_waiter(waiterState);
        }
        return true;
    }

    bool send_batch_to_stream(
        ProducerStream& stream, std::vector<T>&& values) noexcept
    {
        if (!begin_send(stream)) {
            return false;
        }
        if (!reserve_producer_slots(stream, values.size())) {
            finish_send(stream);
            return false;
        }
        ProducerCursor& producer = stream.producer;
        BatchConstructionGuard guard(producer);
        for (T& value : values) {
            if (producer.index == kBlockCapacity) {
                producer.block = producer.block->next.load(std::memory_order_acquire);
                producer.index = 0;
            }
            [[maybe_unused]] T* const stored = std::construct_at(
                producer.block->slots[producer.index].construction_address(),
                std::move(value));
            ++producer.index;
            guard.record_constructed();
        }
        guard.commit();
        TaskState* waiterState = publish_stream(stream, values.size());
        // waiter 已完全摘出，释放 gate 后的调度不再访问 channel/stream。
        finish_send(stream);
        if (waiterState != nullptr) {
            wake_detached_waiter(waiterState);
        }
        return true;
    }

    static std::optional<T> try_pop_stream(ProducerStream& stream) noexcept
    {
        ConsumerCursor& consumer = stream.consumer;
        if (consumer.localConsumed == consumer.observedPublished) {
            consumer.observedPublished =
                stream.shared.published.load(std::memory_order_acquire);
            if (consumer.localConsumed == consumer.observedPublished) {
                consumer.consumed.store(consumer.localConsumed,
                                        std::memory_order_relaxed);
                return std::nullopt;
            }
        }

        if (consumer.index == kBlockCapacity) {
            Block* next = consumer.block->next.load(std::memory_order_acquire);
            if (next == nullptr) {
                return std::nullopt;
            }
            Block* retired = consumer.block;
            consumer.block = next;
            consumer.index = 0;
            recycle_block(stream, retired);
        }

        Block& block = *consumer.block;
        Slot& slot = block.slots[consumer.index];
        T value = std::move(*slot.value());
        std::destroy_at(slot.value());
        ++consumer.index;
        ++consumer.localConsumed;
        consumer.consumed.store(consumer.localConsumed,
                                std::memory_order_relaxed);
        return value;
    }

    void append_ready_streams() noexcept
    {
        if (m_readyStack.load(std::memory_order_seq_cst) == nullptr) {
            return;
        }
        ProducerStream* stack =
            m_readyStack.exchange(nullptr, std::memory_order_seq_cst);
        ProducerStream* ready = nullptr;
        while (stack != nullptr) {
            ProducerStream* next = stack->shared.readyNext;
            stack->shared.readyNext = ready;
            ready = stack;
            stack = next;
        }
        if (ready == nullptr) {
            return;
        }
        ProducerStream* tail = ready;
        size_t appended = 1;
        while (tail->shared.readyNext != nullptr) {
            tail = tail->shared.readyNext;
            ++appended;
        }
        if (m_readyTail == nullptr) {
            m_readyHead = ready;
        } else {
            m_readyTail->shared.readyNext = ready;
        }
        m_readyTail = tail;
        m_activeStreamCount += appended;
    }

    void rotate_ready_head() noexcept
    {
        ProducerStream* current = m_readyHead;
        m_consumerQuotaUsed = 0;
        if (current == nullptr || current->shared.readyNext == nullptr) {
            return;
        }
        m_readyHead = current->shared.readyNext;
        current->shared.readyNext = nullptr;
        m_readyTail->shared.readyNext = current;
        m_readyTail = current;
    }

    std::optional<T> try_pop_next_value() noexcept
    {
        if (m_readyHead == nullptr || m_consumerQuotaUsed == 0) {
            append_ready_streams();
        }
        size_t remaining = m_activeStreamCount;
        while (m_readyHead != nullptr && remaining != 0) {
            ProducerStream* current = m_readyHead;
            auto value = try_pop_stream(*current);
            if (value.has_value()) {
                ++m_consumerQuotaUsed;
                if (m_consumerQuotaUsed >= kConsumerQuota) {
                    rotate_ready_head();
                }
                return value;
            }
            rotate_ready_head();
            --remaining;
        }
        return std::nullopt;
    }

    size_t prefetched_count() const noexcept
    {
        return m_prefetchedVisible.load(std::memory_order_acquire);
    }

    std::optional<T> try_pop_prefetched_value() noexcept
    {
        if (m_prefetchIndex == m_singleRecvPrefetch.size()) {
            return std::nullopt;
        }
        T value = std::move(m_singleRecvPrefetch[m_prefetchIndex]);
        ++m_prefetchIndex;
        const size_t remaining =
            m_singleRecvPrefetch.size() - m_prefetchIndex;
        m_prefetchedVisible.store(remaining, std::memory_order_release);
        if (remaining == 0) {
            m_singleRecvPrefetch.clear();
            m_prefetchIndex = 0;
        }
        return value;
    }

    void try_prefetch_single_recv_values() noexcept
    {
        if (m_singleRecvPrefetchLimit == 0 || m_activeStreamCount != 1 ||
            prefetched_count() != 0) {
            return;
        }
        while (m_singleRecvPrefetch.size() < m_singleRecvPrefetchLimit) {
            auto value = try_pop_next_value();
            if (!value.has_value()) {
                break;
            }
            m_singleRecvPrefetch.push_back(std::move(*value));
        }
        m_prefetchIndex = 0;
        m_prefetchedVisible.store(
            m_singleRecvPrefetch.size(), std::memory_order_release);
    }

    /**
     * @brief waiter arming 后无分配检查是否已有可读消息。
     * @details 首次 stream 激活仍通过 m_readyStack 的 seq_cst 发布/摘取加入 waiter
     *          全序。随后扫描所有已注册 stream：kSending 的 producer 尚未执行
     *          waiter 仲裁，consumer 可继续 arming；kPublished 表示数据和 ready
     *          membership 已发布，必须取消挂起；kOpen 时 acquire published 计数。
     *          gate 与 waiter phase 共用 SC 全序，因此返回 false 后的新发布必由
     *          pending/armed 路径唤醒。
     */
    [[nodiscard]] bool has_published_value_for_waiter() noexcept
    {
        if (prefetched_count() != 0) {
            return true;
        }
        append_ready_streams();
        ProducerStream* stream = m_readyHead;
        while (stream != nullptr) {
            if (stream->consumer.localConsumed !=
                stream->shared.published.load(std::memory_order_acquire)) {
                return true;
            }
            stream = stream->shared.readyNext;
        }

        stream =
            m_streamHead.load(std::memory_order_acquire);
        while (stream != nullptr) {
            const ProducerGate gate =
                stream->control.gate.load(std::memory_order_seq_cst);
            if (gate == ProducerGate::kPublished) {
                return true;
            }
            // 读取 Sending 后再次检查上一轮发布：当前 sender 的 gate 发布同步
            // 此前已完成的同 stream 发送，避免旧数据与本轮预留失败共同留下一个
            // 已经 armed、却再无 producer 仲裁的 waiter。
            if (gate == ProducerGate::kSending) {
                if (stream->consumer.localConsumed !=
                    stream->shared.published.load(std::memory_order_acquire)) {
                    return true;
                }
                stream = stream->next;
                continue;
            }
            if (stream->consumer.localConsumed !=
                stream->shared.published.load(std::memory_order_acquire)) {
                return true;
            }
            stream = stream->next;
        }
        return false;
    }

    /**
     * @brief 完成发布/关闭通知仲裁，并摘出可唤醒 waiter 的注册引用。
     * @return operation 赢得 timeout 仲裁时返回拥有 registration 引用的
     *         TaskState 指针，否则返回 nullptr。
     * @note 返回非空时 registration 已消费、timer 仲裁完成且 phase 已回到 Idle；
     *       arming race 返回空并由唯一 consumer 完成撤销。本函数不调用 scheduler，
     *       因此可在 producer gate 为 kPublished 时安全执行。非空返回值必须且只能
     *       传给 wake_detached_waiter() 一次。
     */
    [[nodiscard]] TaskState* detach_published_waiter() noexcept
    {
        WaiterPhase phase = m_waiterPhase.load(std::memory_order_seq_cst);
        for (;;) {
            if (phase == WaiterPhase::kIdle ||
                phase == WaiterPhase::kArmingPending ||
                phase == WaiterPhase::kWaking) {
                return nullptr;
            }
            if (phase == WaiterPhase::kArmed) {
                return detach_armed_waiter();
            }
            if (m_waiterPhase.compare_exchange_weak(
                    phase,
                    WaiterPhase::kArmingPending,
                    std::memory_order_seq_cst,
                    std::memory_order_seq_cst)) {
                return nullptr;
            }
        }
    }

    /**
     * @brief 在不访问 channel 的情况下接管 registration 引用并调度 waiter。
     * @param waiterState detach_published_waiter() 返回的非空 owning 指针。
     */
    static void wake_detached_waiter(TaskState* waiterState) noexcept
    {
        TaskRef waiterTask =
            kernel::detail::TaskRefStorageAccess::adopt_state(waiterState);
        Waker(std::move(waiterTask)).wake_up();
    }

    bool begin_waiter_registration() noexcept
    {
        WaiterPhase expected = WaiterPhase::kIdle;
        if (!m_waiterPhase.compare_exchange_strong(
                expected,
                WaiterPhase::kArming,
                std::memory_order_seq_cst,
                std::memory_order_seq_cst)) {
            return false;
        }
        m_waiterRegistration.clear_pending_wake();
        return true;
    }

    void cancel_waiter_registration() noexcept
    {
        // 仅由已 begin、尚未 publish 的唯一 consumer 调用。
        WaiterPhase phase = m_waiterPhase.load(std::memory_order_seq_cst);
        while (phase == WaiterPhase::kArming ||
               phase == WaiterPhase::kArmingPending) {
            if (m_waiterPhase.compare_exchange_weak(
                    phase,
                    WaiterPhase::kIdle,
                    std::memory_order_seq_cst,
                    std::memory_order_seq_cst)) {
                return;
            }
        }
    }

    bool publish_waiter(TaskState* waiterState,
                       TimeoutTimer::ptr timeoutTimer = {}) noexcept
    {
        if (waiterState == nullptr) {
            cancel_waiter_registration();
            return false;
        }

        // m_waiterPhase 在线性化点之间转移 m_waiterTimer 的唯一访问权：
        // consumer 在 kArming 写入，kArmed 的 producer/consumer CAS 胜者读取并清空。
        m_waiterTimer = std::move(timeoutTimer);
        TaskRef registration_task(waiterState, true);
        TaskState* registeredState =
            kernel::detail::TaskRefStorageAccess::release_state(registration_task);
        if (!m_waiterRegistration.arm(static_cast<void*>(registeredState))) {
            TaskRef releasedRegistration =
                kernel::detail::TaskRefStorageAccess::adopt_state(registeredState);
            m_waiterTimer.reset();
            cancel_waiter_registration();
            return false;
        }

        WaiterPhase expected = WaiterPhase::kArming;
        if (m_waiterPhase.compare_exchange_strong(
                expected,
                WaiterPhase::kArmed,
                std::memory_order_seq_cst,
                std::memory_order_seq_cst)) {
            return true;
        }

        void* registered = static_cast<void*>(registeredState);
        if (m_waiterRegistration.clear(registered)) {
            TaskRef releasedRegistration =
                kernel::detail::TaskRefStorageAccess::adopt_state(registeredState);
        }
        m_waiterTimer.reset();
        m_waiterRegistration.clear_pending_wake();
        m_waiterPhase.store(WaiterPhase::kIdle, std::memory_order_release);
        return false;
    }

    bool clear_waiter(TaskState* waiterState) noexcept
    {
        if (waiterState == nullptr) {
            return false;
        }
        WaiterPhase expected = WaiterPhase::kArmed;
        if (!m_waiterPhase.compare_exchange_strong(
                expected,
                WaiterPhase::kWaking,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            m_waiterRegistration.clear_pending_wake();
            return false;
        }
        TimeoutTimer::ptr timeoutTimer = std::move(m_waiterTimer);
        void* registered = static_cast<void*>(waiterState);
        const bool cleared = m_waiterRegistration.clear(registered);
        m_waiterRegistration.clear_pending_wake();
        m_waiterPhase.store(WaiterPhase::kIdle, std::memory_order_release);
        if (cleared) {
            TaskRef releasedRegistration =
                kernel::detail::TaskRefStorageAccess::adopt_state(waiterState);
            return true;
        }
        return false;
    }

    [[nodiscard]] TaskState* detach_armed_waiter() noexcept
    {
        WaiterPhase expected = WaiterPhase::kArmed;
        if (!m_waiterPhase.compare_exchange_strong(
                expected,
                WaiterPhase::kWaking,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return nullptr;
        }
        auto* waiterState =
            static_cast<TaskState*>(m_waiterRegistration.consume_wake());
        TimeoutTimer::ptr timeoutTimer = std::move(m_waiterTimer);
        const bool operationWon = waiterState != nullptr &&
            (timeoutTimer == nullptr || timeoutTimer->try_complete_operation());
        m_waiterRegistration.clear_pending_wake();
        m_waiterPhase.store(WaiterPhase::kIdle, std::memory_order_release);
        if (!operationWon) {
            if (waiterState != nullptr) {
                [[maybe_unused]] TaskRef releasedRegistration =
                    kernel::detail::TaskRefStorageAccess::adopt_state(waiterState);
            }
            return nullptr;
        }
        return waiterState;
    }

    alignas(::galay::utils::kCacheLineSize)
        std::atomic<ProducerStream*> m_streamHead{nullptr};
    alignas(::galay::utils::kCacheLineSize)
        std::atomic<ProducerStream*> m_readyStack{nullptr};
    alignas(::galay::utils::kCacheLineSize)
        std::atomic<size_t> m_producerRegistrations{0};
    alignas(::galay::utils::kCacheLineSize)
        std::atomic<WaiterPhase> m_waiterPhase{WaiterPhase::kIdle};
    alignas(::galay::utils::kCacheLineSize)
        std::atomic<size_t> m_prefetchedVisible{0};
    std::vector<T> m_singleRecvPrefetch;
    TimeoutTimer::ptr m_waiterTimer;
    WaitRegistration m_waiterRegistration;
    ProducerStream* m_readyHead = nullptr;
    ProducerStream* m_readyTail = nullptr;
    const uint64_t m_generation;
    const size_t m_defaultBatchSize;
    const size_t m_singleRecvPrefetchLimit;
    size_t m_consumerQuotaUsed = 0;
    size_t m_prefetchIndex = 0;
    size_t m_activeStreamCount = 0;
    alignas(::galay::utils::kCacheLineSize)
        std::atomic<CloseState> m_closeState{CloseState::kOpen};
};

template <UnboundedValue T>
inline bool UnboundedRecvAwaitable<T>::try_receive_now() noexcept
{
    if (m_readyValue.has_value()) {
        return true;
    }
    auto value = m_channel->try_recv();
    if (!value.has_value()) {
        return false;
    }
    // emplace 返回新值引用；状态已由 optional 持有，无需另行使用该别名。
    [[maybe_unused]] T& readyValue = m_readyValue.emplace(std::move(*value));
    return true;
}

template <UnboundedValue T>
inline bool UnboundedRecvAwaitable<T>::await_ready() noexcept
{
    return try_receive_now() || m_channel->is_closed_and_drained();
}

template <UnboundedValue T>
template <typename Promise>
inline bool UnboundedRecvAwaitable<T>::await_suspend(
    std::coroutine_handle<Promise> handle) noexcept
{
    auto* channel = m_channel;
    TimeoutTimer::ptr timeoutTimer = std::move(m_timeoutTimer);
    if (try_receive_now() || channel->is_closed_and_drained()) {
        return false;
    }

    TaskState* waiterState = handle.promise().task_ref_view().state();
    m_waiterState = waiterState;
    if (!channel->begin_waiter_registration()) {
        m_waiterState = nullptr;
        return false;
    }
    if (channel->has_published_value_for_waiter() ||
        channel->is_closed_and_drained()) {
        channel->cancel_waiter_registration();
        m_waiterState = nullptr;
        return false;
    }
    // 发布后生产者可立即恢复并销毁协程帧，因此必须是最后一次访问 awaiter。
    return channel->publish_waiter(waiterState, std::move(timeoutTimer));
}

template <UnboundedValue T>
inline std::expected<T, IOError> UnboundedRecvAwaitable<T>::await_resume() noexcept
{
    if (m_waiterState != nullptr) {
        TaskState* waiterState = m_waiterState;
        if (!m_channel->clear_waiter(waiterState)) {
            // producer/timeout 已消费注册时无需再次释放 waiter 引用。
        }
        m_waiterState = nullptr;
    }
    if (m_timedOut) {
        return std::unexpected(IOError(kTimeout, 0));
    }
    if (m_readyValue.has_value()) {
        return std::move(*m_readyValue);
    }
    auto value = m_channel->try_recv();
    if (value.has_value()) {
        return std::move(*value);
    }
    if (m_channel->is_closed_and_drained()) {
        return std::unexpected(IOError(kClosed, 0));
    }
    return std::unexpected(IOError(kTimeout, 0));
}

template <UnboundedValue T>
inline bool UnboundedRecvBatchAwaitable<T>::try_receive_now()
{
    if (m_readyValues.has_value()) {
        return true;
    }
    auto values = m_channel->try_recv_batch(m_maxCount);
    if (!values.has_value()) {
        return false;
    }
    // emplace 返回新 vector 引用；状态已由 optional 持有。
    [[maybe_unused]] std::vector<T>& readyValues =
        m_readyValues.emplace(std::move(*values));
    return true;
}

template <UnboundedValue T>
inline bool UnboundedRecvBatchAwaitable<T>::await_ready()
{
    return try_receive_now() || m_channel->is_closed_and_drained();
}

template <UnboundedValue T>
template <typename Promise>
inline bool UnboundedRecvBatchAwaitable<T>::await_suspend(
    std::coroutine_handle<Promise> handle)
{
    auto* channel = m_channel;
    TimeoutTimer::ptr timeoutTimer = std::move(m_timeoutTimer);
    if (try_receive_now() || channel->is_closed_and_drained()) {
        return false;
    }

    TaskState* waiterState = handle.promise().task_ref_view().state();
    m_waiterState = waiterState;
    if (!channel->begin_waiter_registration()) {
        m_waiterState = nullptr;
        return false;
    }
    if (channel->has_published_value_for_waiter() ||
        channel->is_closed_and_drained()) {
        channel->cancel_waiter_registration();
        m_waiterState = nullptr;
        return false;
    }
    // 发布后生产者可立即恢复并销毁协程帧，因此必须是最后一次访问 awaiter。
    return channel->publish_waiter(waiterState, std::move(timeoutTimer));
}

template <UnboundedValue T>
inline std::expected<std::vector<T>, IOError>
UnboundedRecvBatchAwaitable<T>::await_resume()
{
    if (m_waiterState != nullptr) {
        TaskState* waiterState = m_waiterState;
        if (!m_channel->clear_waiter(waiterState)) {
            // producer/timeout 已消费注册时无需再次释放 waiter 引用。
        }
        m_waiterState = nullptr;
    }
    if (m_timedOut) {
        return std::unexpected(IOError(kTimeout, 0));
    }
    if (m_readyValues.has_value()) {
        return std::move(*m_readyValues);
    }
    auto values = m_channel->try_recv_batch(m_maxCount);
    if (values.has_value()) {
        return std::move(*values);
    }
    if (m_channel->is_closed_and_drained()) {
        return std::unexpected(IOError(kClosed, 0));
    }
    return std::unexpected(IOError(kTimeout, 0));
}

template <UnboundedValue T>
inline bool UnboundedRecvBatchToAwaitable<T>::try_receive_now() noexcept
{
    if (m_ready || m_invalid) {
        return true;
    }
    if (m_maxCount == 0) {
        m_ready = true;
        return true;
    }
    if (m_destination->size() == m_destination->capacity()) {
        m_invalid = true;
        return true;
    }
    m_readyCount = m_channel->drain_to(*m_destination, m_maxCount);
    m_ready = m_readyCount != 0;
    return m_ready;
}

template <UnboundedValue T>
inline bool UnboundedRecvBatchToAwaitable<T>::await_ready() noexcept
{
    return try_receive_now() || m_channel->is_closed_and_drained();
}

template <UnboundedValue T>
template <typename Promise>
inline bool UnboundedRecvBatchToAwaitable<T>::await_suspend(
    std::coroutine_handle<Promise> handle) noexcept
{
    auto* channel = m_channel;
    TimeoutTimer::ptr timeoutTimer = std::move(m_timeoutTimer);
    if (try_receive_now() || channel->is_closed_and_drained()) {
        return false;
    }

    TaskState* waiterState = handle.promise().task_ref_view().state();
    m_waiterState = waiterState;
    if (!channel->begin_waiter_registration()) {
        m_waiterState = nullptr;
        return false;
    }
    if (channel->has_published_value_for_waiter() ||
        channel->is_closed_and_drained()) {
        channel->cancel_waiter_registration();
        m_waiterState = nullptr;
        return false;
    }
    // 发布后 producer 可立即恢复并销毁协程帧，因此必须是最后一次访问 awaiter。
    return channel->publish_waiter(waiterState, std::move(timeoutTimer));
}

template <UnboundedValue T>
inline std::expected<size_t, IOError>
UnboundedRecvBatchToAwaitable<T>::await_resume() noexcept
{
    if (m_waiterState != nullptr) {
        TaskState* waiterState = m_waiterState;
        const bool cleared = m_channel->clear_waiter(waiterState);
        if (!cleared) {
            // producer/timeout 已消费注册时无需再次释放 waiter 引用。
        }
        m_waiterState = nullptr;
    }
    if (m_timedOut) {
        return std::unexpected(IOError(kTimeout, 0));
    }
    if (!m_ready && !m_invalid) {
        const bool completed = try_receive_now();
        if (!completed && m_channel->is_closed_and_drained()) {
            return std::unexpected(IOError(kClosed, 0));
        }
    }
    if (m_invalid) {
        return std::unexpected(IOError(kParamInvalid, 0));
    }
    if (m_ready) {
        return m_readyCount;
    }
    return std::unexpected(IOError(kTimeout, 0));
}

} // namespace galay::mpsc

#endif // GALAY_MPSC_UNBOUNDED_CHANNEL_H
