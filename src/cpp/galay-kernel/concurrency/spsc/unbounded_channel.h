/**
 * @file unbounded_channel.h
 * @brief 单生产者单消费者无界异步通道
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 使用固定大小分块组成无界 SPSC 队列。异步 waiter 注册与发布使用原子
 * RMW 组成无丢失唤醒握手，稳态不会逐消息分配。单条、批量和攒批接收均支持
 * co_await 与 .timeout()。纯轮询工作负载应使用本头导出的 UnboundedQueue，
 * 避免 waiter 控制面进入逐消息热路径。
 */

#ifndef GALAY_SPSC_UNBOUNDED_CHANNEL_H
#define GALAY_SPSC_UNBOUNDED_CHANNEL_H

#include "../../common/error.h"
#include "../../core/task.h"
#include "../../core/timeout.hpp"
#include "../../core/wait_registration.h"
#include "../../core/waker.h"

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__x86_64__) || defined(__i386__)
#include <emmintrin.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <expected>
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
using kernel::TaskRef;
using kernel::TaskState;
using kernel::TimeoutTimer;
using kernel::TimeoutSupport;
using kernel::WaitRegistration;
using kernel::Waker;
using kernel::WithTimeout;
using kernel::kNotReady;
using kernel::kOutOfMemory;
using kernel::kTimeout;

namespace detail
{

inline void unboundedChannelCpuPause() noexcept
{
#if defined(_MSC_VER)
    YieldProcessor();
#elif defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield" ::: "memory");
#endif
}

template <typename T>
concept UnboundedQueueValue = std::is_object_v<T> &&
    std::same_as<T, std::remove_cv_t<T>> &&
    std::is_nothrow_move_constructible_v<T> &&
    std::is_nothrow_destructible_v<T>;

template <UnboundedQueueValue T>
class UnboundedQueue
{
private:
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__ARM_ARCH_ISA_A64))
    static constexpr size_t kCacheLine = 128;
#else
    static constexpr size_t kCacheLine = 64;
#endif
    static constexpr size_t kBlockTargetBytes = 4096;

    struct Slot
    {
        alignas(T) std::byte storage[sizeof(T)];

        T* storageAddress() noexcept
        {
            return reinterpret_cast<T*>(storage);
        }

        T* value() noexcept
        {
            return std::launder(reinterpret_cast<T*>(storage));
        }
    };

    struct Block;
    static constexpr size_t kBlockPayloadBytes =
        kBlockTargetBytes > sizeof(std::atomic<Block*>)
        ? kBlockTargetBytes - sizeof(std::atomic<Block*>)
        : 0;
    static constexpr size_t kSlotsPerTargetBlock =
        kBlockPayloadBytes / sizeof(Slot);
    static constexpr size_t kBlockCapacity =
        kSlotsPerTargetBlock < 2 ? 2 : kSlotsPerTargetBlock;

    struct Block
    {
        // next 位于块尾，避免 producer 发布后继块时争用 consumer 正在读取的首缓存线。
        std::array<Slot, kBlockCapacity> slots;
        std::atomic<Block*> next{nullptr};
    };

    static_assert(std::atomic<size_t>::is_always_lock_free,
                  "UnboundedQueue requires lock-free size_t atomics");
    static_assert(std::atomic<Block*>::is_always_lock_free,
                  "UnboundedQueue requires lock-free pointer atomics");

    struct alignas(kCacheLine) ProducerPublished
    {
        std::atomic<size_t> value{0};
    };

    struct alignas(kCacheLine) ProducerLocal
    {
        Block* block = nullptr;
        size_t index = 0;
        size_t position = 0;
    };

    struct alignas(kCacheLine) ConsumerLocal
    {
        Block* block = nullptr;
        size_t index = 0;
        size_t position = 0;
        size_t cachedPublished = 0;
    };

public:
    /**
     * @brief 构造一个无 waiter 的分块 SPSC 数据面。
     * @note 首块分配失败时 valid() 返回 false，所有发送操作返回 false。
     */
    UnboundedQueue() noexcept
    {
        Block* first = new (std::nothrow) Block;
        m_producer.block = first;
        m_consumer.block = first;
        m_valid = first != nullptr;
    }

    ~UnboundedQueue() noexcept
    {
        if (m_consumer.block != nullptr) {
            while (tryRecv().has_value()) {
            }
            destroyChain(m_consumer.block);
        }
        Block* recycled = m_recycled.exchange(nullptr, std::memory_order_relaxed);
        destroyChain(recycled);
    }

    UnboundedQueue(const UnboundedQueue&) = delete;
    UnboundedQueue& operator=(const UnboundedQueue&) = delete;
    UnboundedQueue(UnboundedQueue&&) = delete;
    UnboundedQueue& operator=(UnboundedQueue&&) = delete;

    /** @brief 返回首块是否成功分配；该状态在构造后不再变化。 */
    [[nodiscard]] bool valid() const noexcept
    {
        return m_valid;
    }

    /** @brief 返回每个分块的消息容量。 */
    [[nodiscard]] static constexpr size_t blockCapacity() noexcept
    {
        return kBlockCapacity;
    }

    /**
     * @brief 尝试发送一条消息。
     * @return 发布成功返回 true；内存不足返回 false，value 保持未移动。
     * @pre 只有一个 producer 调用流。
     */
    [[nodiscard]] bool send(T&& value) noexcept
    {
        ProducerLocal& producer = m_producer;
        if (producer.block == nullptr) {
            return false;
        }
        if (producer.index == kBlockCapacity) [[unlikely]] {
            if (!prepareNextBlock()) {
                return false;
            }
        }
        [[maybe_unused]] T* const stored = std::construct_at(
            producer.block->slots[producer.index].storageAddress(),
            std::move(value));
        ++producer.index;
        ++producer.position;
        m_published.value.store(producer.position, std::memory_order_release);
        return true;
    }

    /**
     * @brief 移动发送一批消息，并在最后一次性发布游标。
     * @param values 待发送值；成功后全部移动，预分配失败时保持不变。
     */
    [[nodiscard]] bool sendBatch(std::span<T> values) noexcept
    {
        if (values.empty() || !reserveBlocks(values.size())) {
            return values.empty();
        }

        ProducerLocal& producer = m_producer;
        for (T& value : values) {
            if (producer.index == kBlockCapacity) {
                producer.block = producer.block->next.load(std::memory_order_relaxed);
                producer.index = 0;
            }
            [[maybe_unused]] T* const stored = std::construct_at(
                producer.block->slots[producer.index].storageAddress(),
                std::move(value));
            ++producer.index;
            ++producer.position;
        }
        m_published.value.store(producer.position, std::memory_order_release);
        return true;
    }

    /**
     * @brief 尝试接收一条消息。
     * @return 当前无已发布消息时返回空值。
     * @pre 只有一个 consumer 调用流。
     */
    [[nodiscard]] std::optional<T> tryRecv() noexcept
    {
        ConsumerLocal& consumer = m_consumer;
        if (consumer.block == nullptr) {
            return std::nullopt;
        }
        if (consumer.position == consumer.cachedPublished) {
            consumer.cachedPublished = m_published.value.load(std::memory_order_acquire);
            if (consumer.position == consumer.cachedPublished) {
                return std::nullopt;
            }
        }

        if (!advanceConsumerBlockIfNeeded()) {
            return std::nullopt;
        }
        T value = std::move(*consumer.block->slots[consumer.index].value());
        std::destroy_at(consumer.block->slots[consumer.index].value());
        ++consumer.index;
        ++consumer.position;
        return value;
    }

    /**
     * @brief 尝试把一条消息移动到调用方拥有的已构造对象。
     * @param output 接收目标，仅成功时被移动赋值。
     * @return 成功接收返回 true；当前为空返回 false。
     * @pre 只有一个 consumer 调用流。
     */
    [[nodiscard]] bool tryRecv(T& output) noexcept
        requires std::is_nothrow_move_assignable_v<T>
    {
        ConsumerLocal& consumer = m_consumer;
        if (consumer.block == nullptr) {
            return false;
        }
        if (consumer.position == consumer.cachedPublished) {
            consumer.cachedPublished = m_published.value.load(std::memory_order_acquire);
            if (consumer.position == consumer.cachedPublished) {
                return false;
            }
        }

        if (!advanceConsumerBlockIfNeeded()) {
            return false;
        }
        output = std::move(*consumer.block->slots[consumer.index].value());
        std::destroy_at(consumer.block->slots[consumer.index].value());
        ++consumer.index;
        ++consumer.position;
        return true;
    }

    /**
     * @brief 将消息移动到调用方拥有的已构造缓冲区。
     * @return 实际接收条数；0 表示当前为空或 output 为空。
     * @note T 必须可不抛移动赋值；该接口不会分配 vector。
     */
    [[nodiscard]] size_t tryRecvBatch(std::span<T> output) noexcept
        requires std::is_nothrow_move_assignable_v<T>
    {
        if (output.empty() || m_consumer.block == nullptr) {
            return 0;
        }

        ConsumerLocal& consumer = m_consumer;
        if (consumer.position == consumer.cachedPublished) {
            consumer.cachedPublished = m_published.value.load(std::memory_order_acquire);
            if (consumer.position == consumer.cachedPublished) {
                return 0;
            }
        }

        const size_t available = consumer.cachedPublished - consumer.position;
        const size_t count = std::min(output.size(), available);
        size_t received = 0;
        for (; received < count; ++received) {
            if (!advanceConsumerBlockIfNeeded()) {
                break;
            }
            output[received] =
                std::move(*consumer.block->slots[consumer.index].value());
            std::destroy_at(consumer.block->slots[consumer.index].value());
            ++consumer.index;
            ++consumer.position;
        }
        return received;
    }

    /**
     * @brief 返回发布与消费游标之差的消费者侧快照。
     * @pre 只能由唯一逻辑消费者调用，或在两侧都停止后调用。
     */
    [[nodiscard]] size_t size() const noexcept
    {
        const size_t published = m_published.value.load(std::memory_order_acquire);
        return published - m_consumer.position;
    }

    /** @brief 在消费者侧检查当前是否为空。 */
    [[nodiscard]] bool empty() const noexcept
    {
        return size() == 0;
    }

private:
    static void destroyChain(Block* block) noexcept
    {
        while (block != nullptr) {
            Block* next = block->next.load(std::memory_order_relaxed);
            delete block;
            block = next;
        }
    }

    Block* acquireBlock() noexcept
    {
        Block* block = m_recycled.exchange(nullptr, std::memory_order_acquire);
        if (block == nullptr) {
            block = new (std::nothrow) Block;
        }
        return block;
    }

    // 分配与回收只在跨块时发生。冷函数不接收 T&&，避免真实 producer 循环为
    // 每条普通消息物化参数地址；消息构造与游标发布仍留在 send() 的公共热路径。
#if defined(_MSC_VER)
    __declspec(noinline)
#elif defined(__GNUC__) || defined(__clang__)
    __attribute__((noinline, cold))
#endif
    bool prepareNextBlock() noexcept
    {
        ProducerLocal& producer = m_producer;
        Block* next = acquireBlock();
        if (next == nullptr) {
            return false;
        }
        producer.block->next.store(next, std::memory_order_release);
        producer.block = next;
        producer.index = 0;
        return true;
    }

    bool reserveBlocks(size_t count) noexcept
    {
        ProducerLocal& producer = m_producer;
        if (producer.block == nullptr || count == 0) {
            return producer.block != nullptr;
        }

        const size_t remaining = kBlockCapacity - producer.index;
        if (count <= remaining) {
            return true;
        }

        const size_t missing = count - remaining;
        const size_t blocksNeeded =
            missing / kBlockCapacity + (missing % kBlockCapacity != 0 ? 1 : 0);
        Block* first = nullptr;
        Block* last = nullptr;
        for (size_t index = 0; index < blocksNeeded; ++index) {
            Block* block = acquireBlock();
            if (block == nullptr) {
                destroyChain(first);
                return false;
            }
            if (last == nullptr) {
                first = block;
            } else {
                last->next.store(block, std::memory_order_relaxed);
            }
            last = block;
        }
        producer.block->next.store(first, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool advanceConsumerBlockIfNeeded() noexcept
    {
        ConsumerLocal& consumer = m_consumer;
        if (consumer.index != kBlockCapacity) {
            return true;
        }
        // cachedPublished comes from the producer's release publication after
        // the next-block link, so no second acquire is needed at the boundary.
        Block* next = consumer.block->next.load(std::memory_order_relaxed);
        if (next == nullptr) {
            return false;
        }
        Block* retired = consumer.block;
        consumer.block = next;
        consumer.index = 0;
        retired->next.store(nullptr, std::memory_order_relaxed);
        // Only the producer consumes this publication; the displaced pointer
        // was installed earlier by this same consumer.
        Block* displaced = m_recycled.exchange(retired, std::memory_order_release);
        if (displaced != nullptr) {
            delete displaced;
        }
        return true;
    }

    ProducerPublished m_published;
    alignas(kCacheLine) std::atomic<Block*> m_recycled{nullptr};
    ProducerLocal m_producer;
    ConsumerLocal m_consumer;
    bool m_valid = false;
};

} // namespace detail

/** @brief 约束 UnboundedQueue 可在无异常热路径中移动的元素类型。 */
template <typename T>
concept UnboundedQueueValue = detail::UnboundedQueueValue<T>;

/**
 * @brief 无 waiter 的分块 1P1C polling 队列。
 * @tparam T 不抛移动构造和析构的元素类型。
 * @note 只能有一个逻辑生产者和一个逻辑消费者；析构前必须停止两侧访问。
 */
template <UnboundedQueueValue T>
using UnboundedQueue = detail::UnboundedQueue<T>;

/**
 * @brief 约束 UnboundedChannel 可接受的元素类型。
 * @tparam T 元素类型；必须可移动，且移动构造和析构不得抛出异常。
 */
template <typename T>
concept UnboundedValue = std::movable<T> &&
    std::is_nothrow_move_constructible_v<T> &&
    std::is_nothrow_destructible_v<T>;

/**
 * @brief SPSC waiter 的唤醒策略。
 * @details 调度器拥有的任务始终通过 Waker 回到 owner scheduler；Inline 仅为
 *          无 scheduler、同线程手工驱动协程保留直接恢复兼容行为。
 */
enum class WakeMode {
    Inline,   ///< 同线程且无 owner scheduler 时允许直接恢复。
    Deferred, ///< 始终通过 Waker 请求调度器恢复。
};

template <UnboundedValue T>
class UnboundedChannel;

template <UnboundedValue T>
class UnboundedRecvBatchToAwaitable;

/** @brief 单条接收等待体；首块分配失败时同步返回 IOError(kOutOfMemory)。 */
template <UnboundedValue T>
class UnboundedRecvAwaitable : public TimeoutSupport<UnboundedRecvAwaitable<T>>
{
public:
    /** @brief 绑定目标通道；等待完成前通道必须保持有效。 */
    explicit UnboundedRecvAwaitable(UnboundedChannel<T>* channel) noexcept
        : m_channel(channel) {}

    bool await_ready() noexcept;

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) noexcept;

    std::expected<T, IOError> await_resume() noexcept;

private:
    friend struct WithTimeout<UnboundedRecvAwaitable<T>>;

    bool tryReceiveNow() noexcept;
    void markTimeout() noexcept;
    void bindTimeoutTimer(const TimeoutTimer::ptr& timer) noexcept
    {
        m_timeoutTimer = timer;
    }

    UnboundedChannel<T>* m_channel;
    TimeoutTimer::ptr m_timeoutTimer;
    std::optional<T> m_readyValue;
    TaskState* m_waiterState = nullptr;
    bool m_timedOut = false;
    bool m_registrationFailed = false;
};

/**
 * @brief 最多接收指定数量消息并返回独立 vector 的批量等待体。
 * @note 接收路径可能分配，默认 allocator OOM 不经 IOError；要求显式可恢复、
 *       无分配时使用 recvBatchTo(std::span<T>)。
 * @note 首块分配失败时同步返回 IOError(kOutOfMemory)。
 */
template <UnboundedValue T>
class UnboundedRecvBatchAwaitable : public TimeoutSupport<UnboundedRecvBatchAwaitable<T>>
{
public:
    /**
     * @brief 绑定目标通道。
     * @param channel 目标通道；等待完成前必须保持有效。
     * @param maxCount 单次最多接收的消息数。
     */
    UnboundedRecvBatchAwaitable(UnboundedChannel<T>* channel, size_t maxCount) noexcept
        : m_channel(channel), m_maxCount(maxCount) {}

    bool await_ready();

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle);

    std::expected<std::vector<T>, IOError> await_resume();

private:
    friend struct WithTimeout<UnboundedRecvBatchAwaitable<T>>;

    bool tryReceiveNow();
    void markTimeout() noexcept;
    void bindTimeoutTimer(const TimeoutTimer::ptr& timer) noexcept
    {
        m_timeoutTimer = timer;
    }

    UnboundedChannel<T>* m_channel;
    TimeoutTimer::ptr m_timeoutTimer;
    size_t m_maxCount;
    std::optional<std::vector<T>> m_readyValues;
    TaskState* m_waiterState = nullptr;
    bool m_timedOut = false;
    bool m_registrationFailed = false;
};

/**
 * @brief 将批量消息移动到调用方已构造缓冲区的异步等待体。
 * @tparam T 可不抛移动赋值的通道元素类型。
 * @details 空队列时挂起协程而不阻塞线程；数据搬运与 waiter 注册路径不分配内存。
 *          首块分配失败时同步返回 IOError(kOutOfMemory)。
 * @note output 必须活到 await 完成，等待期间不得由其他线程或协程访问。
 */
template <UnboundedValue T>
class UnboundedRecvBatchToAwaitable
    : public TimeoutSupport<UnboundedRecvBatchToAwaitable<T>>
{
public:
    UnboundedRecvBatchToAwaitable(const UnboundedRecvBatchToAwaitable&) = delete;
    UnboundedRecvBatchToAwaitable& operator=(
        const UnboundedRecvBatchToAwaitable&) = delete;
    UnboundedRecvBatchToAwaitable(UnboundedRecvBatchToAwaitable&&) noexcept = default;
    UnboundedRecvBatchToAwaitable& operator=(
        UnboundedRecvBatchToAwaitable&&) noexcept = default;

    /** @brief 尝试立即填充 output；空 span 立即成功。 */
    bool await_ready() noexcept;

    /** @brief 空队列时注册唯一 consumer waiter，不阻塞调度器线程。 */
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) noexcept;

    /**
     * @brief 返回实际搬运数量。
     * @return 成功返回 [0, output.size()]；timeout 获胜返回 IOError(kTimeout, 0)。
     * @note timeout 返回前不会消费消息，也不会修改 output。
     */
    std::expected<size_t, IOError> await_resume() noexcept;

private:
    friend class UnboundedChannel<T>;
    friend struct WithTimeout<UnboundedRecvBatchToAwaitable<T>>;

    UnboundedRecvBatchToAwaitable(UnboundedChannel<T>* channel,
                                  std::span<T> output) noexcept
        : m_output(output), m_channel(channel) {}

    bool tryReceiveNow() noexcept;
    void markTimeout() noexcept { m_timedOut = true; }
    void bindTimeoutTimer(const TimeoutTimer::ptr& timer) noexcept
    {
        m_timeoutTimer = timer;
    }

    std::span<T> m_output;
    TimeoutTimer::ptr m_timeoutTimer;
    UnboundedChannel<T>* m_channel;
    TaskState* m_waiterState = nullptr;
    size_t m_readyCount = 0;
    bool m_ready = false;
    bool m_timedOut = false;
    bool m_registrationFailed = false;
};

/**
 * @brief 达到阈值后接收当前全部消息的攒批等待体。
 * @note timeout 到达时不消费不足阈值的部分消息，并返回 IOError(kTimeout, 0)；
 *       首块分配失败时同步返回 IOError(kOutOfMemory)。
 */
template <UnboundedValue T>
class UnboundedRecvBatchedAwaitable : public TimeoutSupport<UnboundedRecvBatchedAwaitable<T>>
{
public:
    /**
     * @brief 绑定目标通道。
     * @param channel 目标通道；等待完成前必须保持有效。
     * @param limit 正常唤醒前至少需要累积的消息数。
     */
    UnboundedRecvBatchedAwaitable(UnboundedChannel<T>* channel, size_t limit) noexcept
        : m_channel(channel), m_limit(limit) {}

    bool await_ready();

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle);

    std::expected<std::vector<T>, IOError> await_resume();

private:
    friend struct WithTimeout<UnboundedRecvBatchedAwaitable<T>>;

    bool tryReceiveNow();
    void markTimeout() noexcept;
    void bindTimeoutTimer(const TimeoutTimer::ptr& timer) noexcept
    {
        m_timeoutTimer = timer;
    }

    UnboundedChannel<T>* m_channel;
    TimeoutTimer::ptr m_timeoutTimer;
    size_t m_limit;
    std::optional<std::vector<T>> m_readyValues;
    TaskState* m_waiterState = nullptr;
    bool m_timedOut = false;
    bool m_registrationFailed = false;
};

/**
 * @brief 跨线程单生产者单消费者无界异步通道。
 * @tparam T 可移动且移动构造、析构不抛异常的元素类型。
 *
 * @details
 * - 生产者侧只能有一个并发调用流，消费者侧也只能有一个并发调用流。
 * - 使用约 4 KiB 的固定分块，只有跨块时才分配或回收内存。
 * - producer/consumer cursor 分缓存线保存；发布使用 waiter-safe 原子 RMW，
 *   不使用 CAS，也不会逐消息分配。
 * - 异步接收只挂起协程，不阻塞调度器线程。
 *
 * @note 通道不可复制或移动；销毁前不得仍有并发调用或挂起 waiter。
 * @note 首块分配失败时 valid() 返回 false；发送返回 false，异步接收同步返回
 *       IOError(kOutOfMemory)，不会注册一个永远无法完成的 waiter。
 */
template <UnboundedValue T>
class UnboundedChannel
{
public:
    static constexpr size_t DEFAULT_BATCH_SIZE = 1024;

    /**
     * @brief 构造 SPSC 通道并预分配首个分块。
     * @param wakeMode waiter 的兼容唤醒模式。
     * @note 首块分配失败时 valid() 返回 false，send()/sendBatch() 返回 false。
     */
    explicit UnboundedChannel(WakeMode wakeMode = WakeMode::Inline) noexcept
        : m_wakeMode(wakeMode)
    {
        Block* first = new (std::nothrow) Block;
        m_producer.block = first;
        m_consumer.block = first;
        m_valid = first != nullptr;
    }

    /** @brief 销毁所有未消费消息和已分配分块。 */
    ~UnboundedChannel() noexcept
    {
        while (tryRecv().has_value()) {
        }
        destroyBlockChain(m_consumer.block);
    }

    UnboundedChannel(const UnboundedChannel&) = delete;
    UnboundedChannel& operator=(const UnboundedChannel&) = delete;
    UnboundedChannel(UnboundedChannel&&) = delete;
    UnboundedChannel& operator=(UnboundedChannel&&) = delete;

    /** @brief 返回首块是否成功分配；该状态在构造后不再变化。 */
    [[nodiscard]] bool valid() const noexcept
    {
        return m_valid;
    }

    /**
     * @brief 发送一条消息。
     * @param value 待发送值；仅在分块容量准备成功后移动。
     * @param immediately 是否忽略攒批阈值并立即唤醒 waiter。
     * @return 成功发布返回 true；首块或后续分块分配失败返回 false，value 保持未移动。
     * @note 线程安全边界为单个 producer 调用流。
     */
    bool send(T&& value, bool immediately = false)
    {
        if (!reserveProducerSlots(1)) {
            return false;
        }

        ProducerState& producer = m_producer;
        if (producer.index == kBlockCapacity) {
            producer.block = producer.block->next.load(std::memory_order_acquire);
            producer.index = 0;
        }
        [[maybe_unused]] T* const stored = std::construct_at(
            producer.block->slots[producer.index].storageAddress(),
            std::move(value));
        // construct_at 的返回值只回显目标地址，发布由 cursor 完成。
        ++producer.index;

        // Polling-only sends use a release store; once waiter registration has
        // started, publishProducerCount switches to the acq_rel RMW handshake
        // paired with the waiter's fetch_add(0).
        bool waiterPathUsed = false;
        const size_t previous =
            publishProducerCount(producer, 1, waiterPathUsed);
        const size_t published = previous + 1;
        // WaiterPhase publishes waiter state; this monotonic flag only skips the
        // control path before its first use and does not carry waiter data.
        if (immediately ||
            waiterPathUsed) {
            notifyConsumer(previous, published, immediately);
        }
        return true;
    }

    /** @brief 复制并发送一条消息；仅为不抛复制构造类型提供。 */
    bool send(const T& value, bool immediately = false)
        requires std::is_nothrow_copy_constructible_v<T>
    {
        T copy = value;
        return send(std::move(copy), immediately);
    }

    /**
     * @brief 复制并原子发布一批消息。
     * @return 全批发布返回 true；预分配失败返回 false，通道保持不变。
     * @note 仅为不抛复制构造类型提供，避免部分构造后无法显式回滚。
     */
    bool sendBatch(const std::vector<T>& values, bool immediately = false)
        requires std::is_nothrow_copy_constructible_v<T>
    {
        const size_t count = values.size();
        if (count == 0) {
            return true;
        }
        if (!reserveProducerSlots(count)) {
            return false;
        }

        ProducerState& producer = m_producer;
        for (const T& value : values) {
            if (producer.index == kBlockCapacity) {
                producer.block = producer.block->next.load(std::memory_order_acquire);
                producer.index = 0;
            }
            [[maybe_unused]] T* const stored =
                std::construct_at(
                    producer.block->slots[producer.index].storageAddress(),
                    value);
            // construct_at 的返回值只回显目标地址，整批稍后统一发布。
            ++producer.index;
        }

        bool waiterPathUsed = false;
        const size_t previous =
            publishProducerCount(producer, count, waiterPathUsed);
        const size_t published = previous + count;
        if (immediately ||
            waiterPathUsed) {
            notifyConsumer(previous, published, immediately);
        }
        return true;
    }

    /**
     * @brief 移动并原子发布一批消息。
     * @return 全批发布返回 true；预分配失败返回 false，values 保持未移动。
     */
    bool sendBatch(std::vector<T>&& values, bool immediately = false)
    {
        const size_t count = values.size();
        if (count == 0) {
            return true;
        }
        if (!reserveProducerSlots(count)) {
            return false;
        }

        ProducerState& producer = m_producer;
        for (T& value : values) {
            if (producer.index == kBlockCapacity) {
                producer.block = producer.block->next.load(std::memory_order_acquire);
                producer.index = 0;
            }
            [[maybe_unused]] T* const stored = std::construct_at(
                producer.block->slots[producer.index].storageAddress(),
                std::move(value));
            // construct_at 的返回值只回显目标地址，整批稍后统一发布。
            ++producer.index;
        }

        bool waiterPathUsed = false;
        const size_t previous =
            publishProducerCount(producer, count, waiterPathUsed);
        const size_t published = previous + count;
        if (immediately ||
            waiterPathUsed) {
            notifyConsumer(previous, published, immediately);
        }
        return true;
    }

    /** @brief 返回单条异步接收等待体。 */
    UnboundedRecvAwaitable<T> recv() noexcept
    {
        return UnboundedRecvAwaitable<T>(this);
    }

    /**
     * @brief 返回最多接收 maxCount 条消息并拥有独立 vector 的异步等待体。
     * @note 接收路径可能分配，默认 allocator OOM 不经 IOError；要求显式可恢复、
     *       无分配时使用 recvBatchTo(std::span<T>)。
     */
    UnboundedRecvBatchAwaitable<T> recvBatch(size_t maxCount = DEFAULT_BATCH_SIZE) noexcept
    {
        return UnboundedRecvBatchAwaitable<T>(this, maxCount);
    }

    /**
     * @brief 返回把消息移动到调用方已构造缓冲区的异步等待体。
     * @param output 接收目标；空 span 立即成功返回 0。
     * @return 数据搬运与 waiter 注册路径不分配内存的等待体。
     * @note output 必须活到 await 完成，等待期间不得由其他线程或协程访问。
     */
    [[nodiscard]] UnboundedRecvBatchToAwaitable<T> recvBatchTo(
        std::span<T> output) noexcept
        requires std::is_nothrow_move_assignable_v<T>
    {
        return UnboundedRecvBatchToAwaitable<T>(this, output);
    }

    /**
     * @brief 返回达到 limit 后接收当前全部消息并拥有独立 vector 的异步等待体。
     * @note 接收路径可能分配；本接口保留 convenience 语义，默认 allocator OOM
     *       不经 IOError。
     */
    UnboundedRecvBatchedAwaitable<T> recvBatched(size_t limit) noexcept
    {
        return UnboundedRecvBatchedAwaitable<T>(this, limit);
    }

    /**
     * @brief 尝试接收一条消息。
     * @return 成功返回消息；当前为空返回 std::nullopt。
     * @note 线程安全边界为单个 consumer 调用流。
     */
    std::optional<T> tryRecv()
    {
        ConsumerState& consumer = m_consumer;
        const size_t consumed = consumer.consumedValue;
        size_t published = consumer.cachedPublished;
        if (consumed == published) {
            published = m_producer.published.load(std::memory_order_acquire);
            consumer.cachedPublished = published;
        }
        if (consumed == published || consumer.block == nullptr) {
            return std::nullopt;
        }

        if (consumer.index == kBlockCapacity) {
            Block* next = consumer.block->next.load(std::memory_order_acquire);
            if (next == nullptr) {
                return std::nullopt;
            }
            Block* retired = consumer.block;
            consumer.block = next;
            consumer.index = 0;
            delete retired;
        }

        Slot& slot = consumer.block->slots[consumer.index];
        T value = std::move(*slot.value());
        std::destroy_at(slot.value());
        ++consumer.index;
        consumer.consumedValue = consumed + 1;
        consumer.consumed.store(consumer.consumedValue, std::memory_order_release);
        if (m_waiterPathUsed.load(std::memory_order_relaxed)) {
            m_waiter.registration.clearPendingWake();
        }
        return value;
    }

    /**
     * @brief 尝试将消息移动到调用方已构造缓冲区。
     * @param output 接收目标；空 span 不消费消息并返回 0。
     * @return 实际搬运数量，范围为 [0, output.size()]。
     * @note 仅唯一 consumer 可调用；路径不分配，并在整批完成后只发布一次 consumed。
     */
    [[nodiscard]] size_t tryRecvBatch(std::span<T> output) noexcept
        requires std::is_nothrow_move_assignable_v<T>
    {
        if (output.empty()) {
            return 0;
        }

        ConsumerState& consumer = m_consumer;
        const size_t consumed = consumer.consumedValue;
        size_t published = consumer.cachedPublished;
        if (consumed == published) {
            published = m_producer.published.load(std::memory_order_acquire);
            consumer.cachedPublished = published;
        }
        const size_t available = published - consumed;
        if (available == 0 || consumer.block == nullptr) {
            return 0;
        }

        const size_t count = std::min(output.size(), available);
        size_t received = 0;
        for (; received < count; ++received) {
            if (consumer.index == kBlockCapacity) {
                Block* next = consumer.block->next.load(std::memory_order_acquire);
                if (next == nullptr) {
                    break;
                }
                Block* retired = consumer.block;
                consumer.block = next;
                consumer.index = 0;
                delete retired;
            }

            Slot& slot = consumer.block->slots[consumer.index];
            output[received] = std::move(*slot.value());
            std::destroy_at(slot.value());
            ++consumer.index;
        }

        if (received == 0) {
            return 0;
        }
        consumer.consumedValue = consumed + received;
        consumer.consumed.store(consumer.consumedValue, std::memory_order_release);
        if (m_waiterPathUsed.load(std::memory_order_relaxed)) {
            m_waiter.registration.clearPendingWake();
        }
        return received;
    }

    /**
     * @brief 尝试批量接收消息并返回独立 vector。
     * @param maxCount 单次最多接收数量；0 返回空批次。
     * @return 当前有消息时返回批次；为空时返回 std::nullopt。
     * @note 接收路径可能分配，默认 allocator OOM 不经返回值传播；要求显式可恢复、
     *       无分配时使用 tryRecvBatch(std::span<T>)。
     */
    std::optional<std::vector<T>> tryRecvBatch(size_t maxCount = DEFAULT_BATCH_SIZE)
    {
        if (maxCount == 0) {
            return std::vector<T>{};
        }

        ConsumerState& consumer = m_consumer;
        const size_t consumed = consumer.consumedValue;
        size_t published = consumer.cachedPublished;
        if (consumed == published) {
            published = m_producer.published.load(std::memory_order_acquire);
            consumer.cachedPublished = published;
        }
        const size_t available = published - consumed;
        if (available == 0 || consumer.block == nullptr) {
            return std::nullopt;
        }

        const size_t count = std::min(maxCount, available);
        std::vector<T> values;
        values.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            if (consumer.index == kBlockCapacity) {
                Block* next = consumer.block->next.load(std::memory_order_acquire);
                if (next == nullptr) {
                    break;
                }
                Block* retired = consumer.block;
                consumer.block = next;
                consumer.index = 0;
                delete retired;
            }

            Slot& slot = consumer.block->slots[consumer.index];
            values.push_back(std::move(*slot.value()));
            std::destroy_at(slot.value());
            ++consumer.index;
        }

        if (values.empty()) {
            return std::nullopt;
        }
        consumer.consumedValue = consumed + values.size();
        consumer.consumed.store(consumer.consumedValue, std::memory_order_release);
        if (m_waiterPathUsed.load(std::memory_order_relaxed)) {
            m_waiter.registration.clearPendingWake();
        }
        return values;
    }

    /** @brief 返回当前待消费消息数的原子快照。 */
    size_t size() const noexcept
    {
        const size_t published = m_producer.published.load(std::memory_order_acquire);
        const size_t consumed = m_consumer.consumed.load(std::memory_order_acquire);
        return published - consumed;
    }

    /** @brief 检查当前是否为空。 */
    bool empty() const noexcept
    {
        return size() == 0;
    }

private:
#if defined(__aarch64__) || defined(__ARM_ARCH_ISA_A64)
    static constexpr size_t kCacheLine = 128;
#else
    static constexpr size_t kCacheLine = 64;
#endif
    static constexpr size_t kBlockTargetBytes = 4096;

    struct Slot
    {
        alignas(T) std::byte storage[sizeof(T)];

        T* storageAddress() noexcept
        {
            return reinterpret_cast<T*>(storage);
        }

        T* value() noexcept
        {
            return std::launder(reinterpret_cast<T*>(storage));
        }
    };

    struct Block;
    static constexpr size_t kBlockPayloadBytes =
        kBlockTargetBytes > sizeof(std::atomic<Block*>)
        ? kBlockTargetBytes - sizeof(std::atomic<Block*>)
        : 0;
    static constexpr size_t kSlotsPerTargetBlock =
        kBlockPayloadBytes / sizeof(Slot);
    static constexpr size_t kBlockCapacity =
        kSlotsPerTargetBlock < 2 ? 2 : kSlotsPerTargetBlock;

    struct Block
    {
        // next 位于块尾，避免 producer 发布后继块时争用 consumer 正在读取的首缓存线。
        std::array<Slot, kBlockCapacity> slots;
        std::atomic<Block*> next{nullptr};
    };

    struct alignas(kCacheLine) ProducerState
    {
        std::atomic<size_t> published{0};
        Block* block = nullptr;
        size_t index = 0;
        size_t publishedValue = 0;
    };

    struct alignas(kCacheLine) ConsumerState
    {
        std::atomic<size_t> consumed{0};
        Block* block = nullptr;
        size_t index = 0;
        size_t consumedValue = 0;
        size_t cachedPublished = 0;
    };

    enum class WaiterPhase : uint8_t {
        kIdle,
        kRegistering,
        kRegisteringPending,
        kArming,
        kArmingPending,
        kArmed,
        kWaking,
        kCompleted,
    };

    /** @brief 固定 consumer waiter 槽的注册结果。 */
    enum class WaiterArmResult : uint8_t {
        kSuspended,   ///< waiter 已发布，调用方必须保持挂起。
        kReady,       ///< 注册窗口内条件已经满足，调用方应同步重试接收。
        kUnavailable, ///< waiter 槽已占用或注册状态无效，返回 kNotReady。
    };

    struct alignas(kCacheLine) WaitState
    {
        WaitRegistration registration;
        TimeoutTimer::ptr timer;
        std::atomic<TaskState*> owner{nullptr};
        std::atomic<size_t> threshold{1};
        std::atomic<WaiterPhase> phase{WaiterPhase::kIdle};
    };

    template <UnboundedValue U>
    friend class UnboundedRecvAwaitable;
    template <UnboundedValue U>
    friend class UnboundedRecvBatchAwaitable;
    template <UnboundedValue U>
    friend class UnboundedRecvBatchToAwaitable;
    template <UnboundedValue U>
    friend class UnboundedRecvBatchedAwaitable;

    static void destroyBlockChain(Block* block) noexcept
    {
        while (block != nullptr) {
            Block* next = block->next.load(std::memory_order_relaxed);
            delete block;
            block = next;
        }
    }

    /**
     * @brief Publish the producer count without an RMW before waiter mode is used.
     * @details SPSC gives the producer exclusive ownership of the published
     *          counter. A release store is sufficient for polling consumers;
     *          waiter registration switches to the existing acq_rel RMW handshake
     *          before any wake-up can depend on this publication.
     */
    [[nodiscard]] size_t publishProducerCount(ProducerState& producer,
                                              size_t count,
                                              bool& waiterPathUsed) noexcept
    {
        waiterPathUsed = m_waiterPathUsed.load(std::memory_order_relaxed);
        if (waiterPathUsed) {
            const size_t previous =
                producer.published.fetch_add(count, std::memory_order_acq_rel);
            producer.publishedValue = previous + count;
            return previous;
        }

        const size_t previous = producer.publishedValue;
        producer.publishedValue += count;
        producer.published.store(
            producer.publishedValue, std::memory_order_release);
        return previous;
    }

    bool reserveProducerSlots(size_t count) noexcept
    {
        ProducerState& producer = m_producer;
        if (count == 0) {
            return true;
        }
        if (producer.block == nullptr) {
            return false;
        }

        const size_t remaining = kBlockCapacity - producer.index;
        if (count <= remaining) {
            return true;
        }

        const size_t missing = count - remaining;
        const size_t blocksNeeded = missing / kBlockCapacity +
            static_cast<size_t>(missing % kBlockCapacity != 0);
        Block* first = nullptr;
        Block* last = nullptr;
        for (size_t i = 0; i < blocksNeeded; ++i) {
            Block* block = new (std::nothrow) Block;
            if (block == nullptr) {
                destroyBlockChain(first);
                return false;
            }
            if (last == nullptr) {
                first = block;
            } else {
                last->next.store(block, std::memory_order_relaxed);
            }
            last = block;
        }
        producer.block->next.store(first, std::memory_order_release);
        return true;
    }

    WaiterArmResult armWaiter(TaskState* waiterState,
                              size_t threshold,
                              TimeoutTimer::ptr timeoutTimer = {}) noexcept
    {
        if (waiterState == nullptr || !valid()) {
            return WaiterArmResult::kUnavailable;
        }

        WaiterPhase expectedPhase = WaiterPhase::kIdle;
        if (!m_waiter.phase.compare_exchange_strong(
                expectedPhase,
                WaiterPhase::kRegistering,
                std::memory_order_seq_cst,
                std::memory_order_seq_cst)) {
            return WaiterArmResult::kUnavailable;
        }
#ifdef GALAY_SPSC_UNBOUNDED_CLAIMED_TEST_POINT
        GALAY_SPSC_UNBOUNDED_CLAIMED_TEST_POINT();
#endif
        m_waiterPathUsed.store(true, std::memory_order_release);
        m_waiter.owner.store(waiterState, std::memory_order_relaxed);
        threshold = std::max<size_t>(1, threshold);
        m_waiter.threshold.store(threshold, std::memory_order_release);
        m_waiter.registration.clearPendingWake();
        // kRegistering 先独占成员字段；kArming 发布后 producer 才能读取 threshold/timer。
        m_waiter.timer = std::move(timeoutTimer);
#ifdef GALAY_SPSC_UNBOUNDED_REGISTERING_TEST_POINT
        GALAY_SPSC_UNBOUNDED_REGISTERING_TEST_POINT();
#endif
        const size_t firstPublished =
            m_producer.published.fetch_add(0, std::memory_order_acq_rel);
        const size_t firstConsumed =
            m_consumer.consumed.load(std::memory_order_relaxed);
        if (firstPublished - firstConsumed >= threshold) {
            m_waiter.timer.reset();
            m_waiter.threshold.store(1, std::memory_order_release);
            m_waiter.owner.store(nullptr, std::memory_order_relaxed);
            m_waiter.phase.store(WaiterPhase::kIdle, std::memory_order_seq_cst);
            return WaiterArmResult::kReady;
        }
        TaskRef registrationTask(waiterState, true);
        TaskState* registeredState =
            kernel::detail::TaskRefStorageAccess::releaseState(registrationTask);
        if (!m_waiter.registration.arm(static_cast<void*>(registeredState))) {
            TaskRef releasedRegistration =
                kernel::detail::TaskRefStorageAccess::adoptState(registeredState);
            m_waiter.timer.reset();
            m_waiter.threshold.store(1, std::memory_order_release);
            m_waiter.owner.store(nullptr, std::memory_order_relaxed);
            m_waiter.phase.store(WaiterPhase::kIdle, std::memory_order_seq_cst);
            return WaiterArmResult::kUnavailable;
        }

        expectedPhase = WaiterPhase::kRegistering;
        if (!m_waiter.phase.compare_exchange_strong(
                expectedPhase,
                WaiterPhase::kArming,
                std::memory_order_seq_cst,
                std::memory_order_seq_cst)) {
            void* registered = static_cast<void*>(registeredState);
            if (m_waiter.registration.clear(registered)) {
                TaskRef releasedRegistration =
                    kernel::detail::TaskRefStorageAccess::adoptState(registeredState);
            }
            m_waiter.timer.reset();
            m_waiter.registration.clearPendingWake();
            m_waiter.threshold.store(1, std::memory_order_release);
            m_waiter.owner.store(nullptr, std::memory_order_relaxed);
            m_waiter.phase.store(WaiterPhase::kIdle, std::memory_order_seq_cst);
            return expectedPhase == WaiterPhase::kRegisteringPending
                ? WaiterArmResult::kReady
                : WaiterArmResult::kUnavailable;
        }

        const size_t secondPublished =
            m_producer.published.fetch_add(0, std::memory_order_acq_rel);
        const size_t secondConsumed =
            m_consumer.consumed.load(std::memory_order_relaxed);
        if (secondPublished - secondConsumed >= threshold) {
            void* expected = static_cast<void*>(registeredState);
            if (m_waiter.registration.clear(expected)) {
                TaskRef releasedRegistration =
                    kernel::detail::TaskRefStorageAccess::adoptState(registeredState);
            }
            m_waiter.timer.reset();
            m_waiter.registration.clearPendingWake();
            m_waiter.threshold.store(1, std::memory_order_release);
            m_waiter.owner.store(nullptr, std::memory_order_relaxed);
            m_waiter.phase.store(WaiterPhase::kIdle, std::memory_order_seq_cst);
            return WaiterArmResult::kReady;
        }

        expectedPhase = WaiterPhase::kArming;
        if (m_waiter.phase.compare_exchange_strong(
                expectedPhase,
                WaiterPhase::kArmed,
                std::memory_order_seq_cst,
                std::memory_order_seq_cst)) {
            return WaiterArmResult::kSuspended;
        }

        void* registered = static_cast<void*>(registeredState);
        if (m_waiter.registration.clear(registered)) {
            TaskRef releasedRegistration =
                kernel::detail::TaskRefStorageAccess::adoptState(registeredState);
        }
        m_waiter.timer.reset();
        m_waiter.registration.clearPendingWake();
        m_waiter.threshold.store(1, std::memory_order_release);
        m_waiter.owner.store(nullptr, std::memory_order_relaxed);
        m_waiter.phase.store(WaiterPhase::kIdle, std::memory_order_seq_cst);
        return WaiterArmResult::kReady;
    }

    bool clearWaiter(TaskState* waiterState) noexcept
    {
        if (waiterState == nullptr) {
            return false;
        }
        if (m_waiter.owner.load(std::memory_order_acquire) != waiterState) {
            return false;
        }

        WaiterPhase phase = m_waiter.phase.load(std::memory_order_acquire);
        for (;;) {
            if (phase == WaiterPhase::kCompleted) {
                TaskState* expectedOwner = waiterState;
                if (!m_waiter.owner.compare_exchange_strong(
                        expectedOwner,
                        nullptr,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed)) {
                    return false;
                }
                // Idle 是唯一允许下一个 awaiter 复用 timer/threshold/registration 的发布点。
                m_waiter.phase.store(WaiterPhase::kIdle, std::memory_order_release);
                return true;
            }
            if (phase == WaiterPhase::kArmed) {
                if (!m_waiter.phase.compare_exchange_weak(
                        phase,
                        WaiterPhase::kWaking,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire)) {
                    continue;
                }

                // 当前 awaiter 主动回收尚未被 producer 认领的注册。
                TimeoutTimer::ptr timeoutTimer = std::move(m_waiter.timer);
                void* registered = static_cast<void*>(waiterState);
                if (m_waiter.registration.clear(registered)) {
                    TaskRef releasedRegistration =
                        kernel::detail::TaskRefStorageAccess::adoptState(waiterState);
                }
                m_waiter.registration.clearPendingWake();
                m_waiter.threshold.store(1, std::memory_order_release);
                m_waiter.phase.store(WaiterPhase::kCompleted,
                                     std::memory_order_release);
                phase = WaiterPhase::kCompleted;
                continue;
            }
            if (phase == WaiterPhase::kIdle) {
                return false;
            }
            if (phase != WaiterPhase::kWaking) {
                // await_resume 只能与已发布的 Armed、Waking 或 Completed 交错。
                // 其余 phase 表示注册状态机契约已被破坏，不能永久占住执行线程。
                return false;
            }

            // producer/timeout completion owner only performs finite registration cleanup.
#ifdef GALAY_SPSC_UNBOUNDED_RECLAIM_WAIT_TEST_POINT
            GALAY_SPSC_UNBOUNDED_RECLAIM_WAIT_TEST_POINT();
#endif
            detail::unboundedChannelCpuPause();
            phase = m_waiter.phase.load(std::memory_order_acquire);
        }
    }

    void wakePublishedWaiter() noexcept
    {
        WaiterPhase phase = m_waiter.phase.load(std::memory_order_seq_cst);
        for (;;) {
            if (phase == WaiterPhase::kIdle ||
                phase == WaiterPhase::kRegistering ||
                phase == WaiterPhase::kRegisteringPending ||
                phase == WaiterPhase::kArmingPending ||
                phase == WaiterPhase::kWaking ||
                phase == WaiterPhase::kCompleted) {
                return;
            }
            if (phase == WaiterPhase::kArming) {
                if (m_waiter.phase.compare_exchange_weak(
                        phase,
                        WaiterPhase::kArmingPending,
                        std::memory_order_seq_cst,
                        std::memory_order_seq_cst)) {
                    return;
                }
                continue;
            }
            if (m_waiter.phase.compare_exchange_weak(
                    phase,
                    WaiterPhase::kWaking,
                    std::memory_order_seq_cst,
                    std::memory_order_seq_cst)) {
                break;
            }
        }

#ifdef GALAY_SPSC_UNBOUNDED_WAKING_TEST_POINT
        GALAY_SPSC_UNBOUNDED_WAKING_TEST_POINT();
#endif
        auto* waiterState =
            static_cast<TaskState*>(m_waiter.registration.consumeWake());
        TaskRef waiterTask;
        if (waiterState != nullptr) {
            waiterTask = kernel::detail::TaskRefStorageAccess::adoptState(waiterState);
        }
        TimeoutTimer::ptr timeoutTimer = std::move(m_waiter.timer);
        TaskState* const owner = m_waiter.owner.load(std::memory_order_acquire);
        const bool operationWon = waiterState != nullptr && waiterState == owner &&
            (timeoutTimer == nullptr || timeoutTimer->tryCompleteOperation());
        const WakeMode wakeMode = m_wakeMode;
        m_waiter.registration.clearPendingWake();
        m_waiter.threshold.store(1, std::memory_order_release);
        // Completed 是完成方最后一次 channel 访问；原 await_resume 负责回收为 Idle。
        m_waiter.phase.store(WaiterPhase::kCompleted, std::memory_order_release);
        if (!operationWon) {
            return;
        }

        if (wakeMode == WakeMode::Inline && waiterState->m_scheduler == nullptr &&
            waiterState->m_handle) {
            waiterState->m_handle.resume();
            return;
        }
        Waker(std::move(waiterTask)).wakeUp();
    }

    void notifyConsumer(size_t previousPublished,
                        size_t published,
                        bool immediately) noexcept
    {
        WaiterPhase phase = m_waiter.phase.load(std::memory_order_seq_cst);
        if (phase == WaiterPhase::kRegistering) {
            if (!immediately) {
                return;
            }
            // notify 可能在发布后被抢占；若 consumer 已消费到本次 published，
            // 这是旧消息的延迟通知，不能把后续 generation 标成 pending。
            const size_t consumed =
                m_consumer.consumed.load(std::memory_order_acquire);
            if (published - consumed == 0) {
                return;
            }
            if (m_waiter.phase.compare_exchange_strong(
                    phase,
                    WaiterPhase::kRegisteringPending,
                    std::memory_order_seq_cst,
                    std::memory_order_seq_cst)) {
                return;
            }
        }
        if (phase == WaiterPhase::kIdle ||
            phase == WaiterPhase::kRegisteringPending ||
            phase == WaiterPhase::kArmingPending ||
            phase == WaiterPhase::kWaking ||
            phase == WaiterPhase::kCompleted) {
            return;
        }
        const size_t threshold = m_waiter.threshold.load(std::memory_order_acquire);
        const size_t consumed = m_consumer.consumed.load(std::memory_order_acquire);
        const size_t previousAvailable = previousPublished - consumed;
        const size_t currentAvailable = published - consumed;
        if (currentAvailable == 0) {
            return;
        }
        if (immediately ||
            (previousAvailable < threshold && currentAvailable >= threshold)) {
            wakePublishedWaiter();
            return;
        }

        if (m_waiter.registration.hasWaiter()) {
            const size_t currentConsumed =
                m_consumer.consumed.load(std::memory_order_acquire);
            const size_t remainingAvailable = published - currentConsumed;
            if (remainingAvailable != 0 &&
                (immediately || remainingAvailable >= threshold)) {
                wakePublishedWaiter();
            }
        }
    }

    ProducerState m_producer;
    ConsumerState m_consumer;
    WaitState m_waiter;
    std::atomic<bool> m_waiterPathUsed{false};
    WakeMode m_wakeMode;
    bool m_valid = false;
};

template <UnboundedValue T>
inline bool UnboundedRecvAwaitable<T>::tryReceiveNow() noexcept
{
    if (m_readyValue.has_value()) {
        return true;
    }
    auto value = m_channel->tryRecv();
    if (!value.has_value()) {
        return false;
    }
    m_readyValue.emplace(std::move(*value));
    return true;
}

template <UnboundedValue T>
inline bool UnboundedRecvAwaitable<T>::await_ready() noexcept
{
    return tryReceiveNow() || !m_channel->valid();
}

template <UnboundedValue T>
template <typename Promise>
inline bool UnboundedRecvAwaitable<T>::await_suspend(
    std::coroutine_handle<Promise> handle) noexcept
{
    auto* channel = m_channel;
    TimeoutTimer::ptr timeoutTimer = std::move(m_timeoutTimer);
    if (tryReceiveNow()) {
        return false;
    }
    if (!channel->valid()) {
        return false;
    }
    TaskState* waiterState = handle.promise().taskRefView().state();
    m_waiterState = waiterState;
    using ArmResult = typename UnboundedChannel<T>::WaiterArmResult;
    const ArmResult result =
        channel->armWaiter(waiterState, 1, std::move(timeoutTimer));
    if (result == ArmResult::kSuspended) {
        // producer 此后可立即恢复并销毁协程帧，不得再访问 awaiter/channel。
        return true;
    }
    m_waiterState = nullptr;
    m_registrationFailed = result == ArmResult::kUnavailable;
    return false;
}

template <UnboundedValue T>
inline void UnboundedRecvAwaitable<T>::markTimeout() noexcept
{
    m_timedOut = true;
}

template <UnboundedValue T>
inline std::expected<T, IOError> UnboundedRecvAwaitable<T>::await_resume() noexcept
{
    if (m_registrationFailed) {
        return std::unexpected(IOError(kNotReady, 0));
    }
    if (m_waiterState != nullptr) {
        TaskState* waiterState = m_waiterState;
        if (!m_channel->clearWaiter(waiterState)) {
            // producer/timeout 已消费注册时无需再次释放 waiter 引用。
        }
        m_waiterState = nullptr;
    }
    if (!m_channel->valid()) {
        return std::unexpected(IOError(kOutOfMemory, 0));
    }
    if (m_timedOut) {
        return std::unexpected(IOError(kTimeout, 0));
    }
    if (m_readyValue.has_value()) {
        return std::move(*m_readyValue);
    }
    auto value = m_channel->tryRecv();
    if (value.has_value()) {
        return std::move(*value);
    }
    return std::unexpected(IOError(kNotReady, 0));
}

template <UnboundedValue T>
inline bool UnboundedRecvBatchToAwaitable<T>::tryReceiveNow() noexcept
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

template <UnboundedValue T>
inline bool UnboundedRecvBatchToAwaitable<T>::await_ready() noexcept
{
    return tryReceiveNow() || !m_channel->valid();
}

template <UnboundedValue T>
template <typename Promise>
inline bool UnboundedRecvBatchToAwaitable<T>::await_suspend(
    std::coroutine_handle<Promise> handle) noexcept
{
    auto* channel = m_channel;
    TimeoutTimer::ptr timeoutTimer = std::move(m_timeoutTimer);
    if (tryReceiveNow()) {
        return false;
    }
    if (!channel->valid()) {
        return false;
    }
    TaskState* waiterState = handle.promise().taskRefView().state();
    m_waiterState = waiterState;
    using ArmResult = typename UnboundedChannel<T>::WaiterArmResult;
    const ArmResult result =
        channel->armWaiter(waiterState, 1, std::move(timeoutTimer));
    if (result == ArmResult::kSuspended) {
        // producer 此后可立即恢复并销毁协程帧，不得再访问 awaiter/channel。
        return true;
    }
    m_waiterState = nullptr;
    m_registrationFailed = result == ArmResult::kUnavailable;
    return false;
}

template <UnboundedValue T>
inline std::expected<size_t, IOError>
UnboundedRecvBatchToAwaitable<T>::await_resume() noexcept
{
    if (m_registrationFailed) {
        return std::unexpected(IOError(kNotReady, 0));
    }
    if (m_waiterState != nullptr) {
        TaskState* waiterState = m_waiterState;
        if (!m_channel->clearWaiter(waiterState)) {
            // producer/timeout 已消费注册时无需再次释放 waiter 引用。
        }
        m_waiterState = nullptr;
    }
    if (!m_channel->valid()) {
        return std::unexpected(IOError(kOutOfMemory, 0));
    }
    if (m_timedOut) {
        return std::unexpected(IOError(kTimeout, 0));
    }
    if (tryReceiveNow()) {
        return m_readyCount;
    }
    return std::unexpected(IOError(kNotReady, 0));
}

template <UnboundedValue T>
inline bool UnboundedRecvBatchAwaitable<T>::tryReceiveNow()
{
    if (m_readyValues.has_value()) {
        return true;
    }
    auto values = m_channel->tryRecvBatch(m_maxCount);
    if (!values.has_value()) {
        return false;
    }
    m_readyValues.emplace(std::move(*values));
    return true;
}

template <UnboundedValue T>
inline bool UnboundedRecvBatchAwaitable<T>::await_ready()
{
    return tryReceiveNow() || !m_channel->valid();
}

template <UnboundedValue T>
template <typename Promise>
inline bool UnboundedRecvBatchAwaitable<T>::await_suspend(
    std::coroutine_handle<Promise> handle)
{
    auto* channel = m_channel;
    TimeoutTimer::ptr timeoutTimer = std::move(m_timeoutTimer);
    if (tryReceiveNow()) {
        return false;
    }
    if (!channel->valid()) {
        return false;
    }
    TaskState* waiterState = handle.promise().taskRefView().state();
    m_waiterState = waiterState;
    using ArmResult = typename UnboundedChannel<T>::WaiterArmResult;
    const ArmResult result =
        channel->armWaiter(waiterState, 1, std::move(timeoutTimer));
    if (result == ArmResult::kSuspended) {
        // producer 此后可立即恢复并销毁协程帧，不得再访问 awaiter/channel。
        return true;
    }
    m_waiterState = nullptr;
    m_registrationFailed = result == ArmResult::kUnavailable;
    return false;
}

template <UnboundedValue T>
inline void UnboundedRecvBatchAwaitable<T>::markTimeout() noexcept
{
    m_timedOut = true;
}

template <UnboundedValue T>
inline std::expected<std::vector<T>, IOError>
UnboundedRecvBatchAwaitable<T>::await_resume()
{
    if (m_registrationFailed) {
        return std::unexpected(IOError(kNotReady, 0));
    }
    if (m_waiterState != nullptr) {
        TaskState* waiterState = m_waiterState;
        if (!m_channel->clearWaiter(waiterState)) {
            // producer/timeout 已消费注册时无需再次释放 waiter 引用。
        }
        m_waiterState = nullptr;
    }
    if (!m_channel->valid()) {
        return std::unexpected(IOError(kOutOfMemory, 0));
    }
    if (m_timedOut) {
        return std::unexpected(IOError(kTimeout, 0));
    }
    if (m_readyValues.has_value()) {
        return std::move(*m_readyValues);
    }
    auto values = m_channel->tryRecvBatch(m_maxCount);
    if (values.has_value()) {
        return std::move(*values);
    }
    return std::unexpected(IOError(kNotReady, 0));
}

template <UnboundedValue T>
inline bool UnboundedRecvBatchedAwaitable<T>::tryReceiveNow()
{
    if (m_readyValues.has_value()) {
        return true;
    }
    const size_t count = m_channel->size();
    if (count < m_limit) {
        return false;
    }
    if (count == 0) {
        m_readyValues.emplace();
        return true;
    }
    auto values = m_channel->tryRecvBatch(count);
    if (!values.has_value()) {
        return false;
    }
    m_readyValues.emplace(std::move(*values));
    return true;
}

template <UnboundedValue T>
inline bool UnboundedRecvBatchedAwaitable<T>::await_ready()
{
    return tryReceiveNow() || !m_channel->valid();
}

template <UnboundedValue T>
template <typename Promise>
inline bool UnboundedRecvBatchedAwaitable<T>::await_suspend(
    std::coroutine_handle<Promise> handle)
{
    auto* channel = m_channel;
    TimeoutTimer::ptr timeoutTimer = std::move(m_timeoutTimer);
    if (tryReceiveNow()) {
        return false;
    }
    if (!channel->valid()) {
        return false;
    }
    TaskState* waiterState = handle.promise().taskRefView().state();
    m_waiterState = waiterState;
    const size_t limit = m_limit;
    using ArmResult = typename UnboundedChannel<T>::WaiterArmResult;
    const ArmResult result =
        channel->armWaiter(waiterState, limit, std::move(timeoutTimer));
    if (result == ArmResult::kSuspended) {
        // producer 此后可立即恢复并销毁协程帧，不得再访问 awaiter/channel。
        return true;
    }
    m_waiterState = nullptr;
    m_registrationFailed = result == ArmResult::kUnavailable;
    return false;
}

template <UnboundedValue T>
inline void UnboundedRecvBatchedAwaitable<T>::markTimeout() noexcept
{
    m_timedOut = true;
}

template <UnboundedValue T>
inline std::expected<std::vector<T>, IOError>
UnboundedRecvBatchedAwaitable<T>::await_resume()
{
    if (m_registrationFailed) {
        return std::unexpected(IOError(kNotReady, 0));
    }
    if (m_waiterState != nullptr) {
        TaskState* waiterState = m_waiterState;
        if (!m_channel->clearWaiter(waiterState)) {
            // producer/timeout 已消费注册时无需再次释放 waiter 引用。
        }
        m_waiterState = nullptr;
    }
    if (!m_channel->valid()) {
        return std::unexpected(IOError(kOutOfMemory, 0));
    }
    if (m_timedOut) {
        return std::unexpected(IOError(kTimeout, 0));
    }
    if (m_readyValues.has_value()) {
        return std::move(*m_readyValues);
    }

    const size_t count = m_channel->size();
    if (count == 0) {
        return std::unexpected(IOError(kNotReady, 0));
    }
    auto values = m_channel->tryRecvBatch(count);
    if (values.has_value()) {
        return std::move(*values);
    }
    return std::unexpected(IOError(kNotReady, 0));
}

} // namespace galay::spsc

#endif // GALAY_SPSC_UNBOUNDED_CHANNEL_H
