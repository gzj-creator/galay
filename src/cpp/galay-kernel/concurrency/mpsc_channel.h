/**
 * @file mpsc_channel.h
 * @brief 多生产者单消费者异步通道
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 无锁、线程安全的异步通道，支持多个生产者线程和单个消费者协程。
 * 底层存储基于 moodycamel::ConcurrentQueue。提供单条和批量接收操作，
 * 均兼容 co_await，并可选 .timeout() 链式调用。
 *
 * 特性：
 * - 任意数量的生产者线程均可安全发送
 * - 单消费者协程接收
 * - 批量发送与接收
 * - 单条接收的预取缓存，减少队列竞争
 * - 通过 TimeoutSupport 可选超时支持
 */

#ifndef GALAY_KERNEL_MPSC_CHANNEL_H
#define GALAY_KERNEL_MPSC_CHANNEL_H

#include "../core/task.h"
#include "../core/timeout.hpp"
#include "../core/wait_registration.h"
#include "../core/waker.h"
#include "../common/error.h"
#include <algorithm>
#include <concurrentqueue/moodycamel/concurrentqueue.h>
#include <atomic>
#include <concepts>
#include <coroutine>
#include <expected>
#include <optional>
#include <vector>
#include <thread>

namespace galay::kernel
{

/**
 * @brief 约束 MpscChannel 可接受的元素类型
 * @tparam T 元素类型；必须可移动且可默认构造
 */
template <typename T>
concept MpscValue = std::movable<T> && std::default_initializable<T>;

template <typename T>
class MpscChannel;

struct MpscChannelTestAccess;

/**
 * @brief 单条接收的等待体
 * @tparam T 通道元素类型
 */
template <typename T>
class MpscRecvAwaitable : public TimeoutSupport<MpscRecvAwaitable<T>>
{
public:
    static_assert(MpscValue<T>, "MpscRecvAwaitable requires movable and default initializable T");

    /**
     * @brief 构造单条接收等待体
     * @param channel 关联的通道对象；等待完成前必须保持有效
     */
    explicit MpscRecvAwaitable(MpscChannel<T>* channel) : m_channel(channel) {}

    bool await_ready() noexcept;  ///< 如果当前已经能取到数据，则返回 true 避免挂起
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) noexcept;  ///< 注册单消费者等待者并在通道为空时挂起
    std::expected<T, IOError> await_resume() noexcept;  ///< 返回单条消息；超时则返回 IOError(kTimeout, 0)

private:
    friend struct WithTimeout<MpscRecvAwaitable<T>>;
    bool tryReceiveNow() noexcept;

    MpscChannel<T>* m_channel;
    std::optional<T> m_readyValue;
    TaskState* m_waiterState = nullptr;
};

/**
 * @brief 批量接收的等待体
 * @tparam T 通道元素类型
 */
template <typename T>
class MpscRecvBatchAwaitable : public TimeoutSupport<MpscRecvBatchAwaitable<T>>
{
public:
    static_assert(MpscValue<T>, "MpscRecvBatchAwaitable requires movable and default initializable T");

    /**
     * @brief 构造批量接收等待体
     * @param channel 关联的通道对象；等待完成前必须保持有效
     * @param max_count 单次恢复时最多返回的元素数
     */
    explicit MpscRecvBatchAwaitable(MpscChannel<T>* channel, size_t max_count)
        : m_channel(channel), m_maxCount(max_count){}

    bool await_ready() noexcept;  ///< 如果当前已经能取到数据，则返回 true 避免挂起
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) noexcept;  ///< 注册单消费者等待者并在通道为空时挂起
    std::expected<std::vector<T>, IOError> await_resume() noexcept;  ///< 返回最多 m_maxCount 条消息；超时则返回 IOError(kTimeout, 0)

private:
    friend struct WithTimeout<MpscRecvBatchAwaitable<T>>;
    bool tryReceiveNow() noexcept;

    MpscChannel<T>* m_channel;
    size_t m_maxCount;
    std::optional<std::vector<T>> m_readyValues;
    TaskState* m_waiterState = nullptr;
};

/**
 * @brief 多生产者单消费者异步通道
 * @tparam T 通道元素类型，要求可移动且可默认构造
 * @details 发送侧线程安全，接收侧假定只有单个消费者协程或线程。
 */
template <typename T>
class MpscChannel
{
public:
    static_assert(MpscValue<T>, "MpscChannel requires movable and default initializable T");
    using MpscToken = std::thread::id;  ///< 生产者标识类型，当前保留为线程 ID

    /**
     * @brief 构造 MPSC 通道
     * @param defaultBatchSize 未显式指定时，批量接收默认最多返回的元素数
     * @param singleRecvPrefetchLimit 单条接收时预取缓存的上限，0 表示禁用预取
     */
    explicit MpscChannel(size_t defaultBatchSize = 1024, size_t singleRecvPrefetchLimit = 16)
        : m_defaultBatchSize(std::max<size_t>(1, defaultBatchSize))
        , m_singleRecvPrefetchLimit(singleRecvPrefetchLimit)
        , m_singleRecvPrefetch(singleRecvPrefetchLimit) {}
    MpscChannel(const MpscChannel&) = delete;
    MpscChannel& operator=(const MpscChannel&) = delete;
    MpscChannel(MpscChannel&&) = delete;
    MpscChannel& operator=(MpscChannel&&) = delete;

    /**
     * @brief 发送单条数据（线程安全，支持跨调度器）,保证一定有对端在recv
     * @param value 待发送的数据
     * @return true 入队成功；false 底层无锁队列拒绝入队
     */
    bool send(T&& value) {
        uint32_t prevSize = m_size.fetch_add(1, std::memory_order_acq_rel);
        if (!m_queue.enqueue(std::forward<T>(value))) {
            m_size.fetch_sub(1, std::memory_order_acq_rel);
            return false;
        }
        if (prevSize == 0 || hasWaiter()) {
            wakePublishedWaiter();
        }
        return true;
    }

    /**
     * @brief 复制发送单条数据
     * @param value 待复制进通道的数据
     * @return true 入队成功；false 底层无锁队列拒绝入队
     */
    bool send(const T& value) requires std::copy_constructible<T> {
        T copy = value;
        return send(std::move(copy));
    }

    /**
     * @brief 批量发送数据
     * @param values 待复制进通道的一批数据
     * @return true 全部入队成功；false 底层无锁队列拒绝本次批量入队
     */
    bool sendBatch(const std::vector<T>& values) requires std::copy_constructible<T> {
        if (values.empty()) return true;
        uint32_t count = static_cast<uint32_t>(values.size());
        uint32_t prevSize = m_size.fetch_add(count, std::memory_order_acq_rel);
        if (!m_queue.enqueue_bulk(values.data(), values.size())) {
            m_size.fetch_sub(count, std::memory_order_acq_rel);
            return false;
        }
        if (prevSize == 0 || hasWaiter()) {
            wakePublishedWaiter();
        }
        return true;
    }

    /**
     * @brief 移动发送一批数据
     * @param values 待移动进通道的一批数据
     * @return true 全部入队成功；false 底层无锁队列拒绝本次批量入队
     */
    bool sendBatch(std::vector<T>&& values) {
        if (values.empty()) return true;
        uint32_t count = static_cast<uint32_t>(values.size());
        uint32_t prevSize = m_size.fetch_add(count, std::memory_order_acq_rel);
        if (!m_queue.enqueue_bulk(std::make_move_iterator(values.begin()), count)) {
            m_size.fetch_sub(count, std::memory_order_acq_rel);
            return false;
        }
        if (prevSize == 0 || hasWaiter()) {
            wakePublishedWaiter();
        }
        return true;
    }

    /**
     * @brief 异步接收单条数据
     * @return 可 `co_await` 的等待体；若叠加 `.timeout(...)`，超时后返回 IOError
     */
    MpscRecvAwaitable<T> recv() {
        return MpscRecvAwaitable<T>(this);
    }

    /**
     * @brief 按默认批量大小异步接收数据
     * @return 可 `co_await` 的等待体；恢复后返回最多 `defaultBatchSize` 条数据
     */
    MpscRecvBatchAwaitable<T> recvBatch() {
        return MpscRecvBatchAwaitable<T>(this, m_defaultBatchSize);
    }

    /**
     * @brief 按指定批量大小异步接收数据
     * @param maxCount 单次最多返回的元素数
     * @return 可 `co_await` 的等待体；恢复后返回最多 `maxCount` 条数据
     */
    MpscRecvBatchAwaitable<T> recvBatch(size_t maxCount) {
        return MpscRecvBatchAwaitable<T>(this, maxCount);
    }

    /**
     * @brief 非阻塞接收单条数据
     * @return 取到数据时返回该值；通道为空时返回 std::nullopt
     */
    std::optional<T> tryRecv() {
        if (auto cached = tryPopPrefetchedValue(); cached.has_value()) {
            return cached;
        }
        if (m_size.load(std::memory_order_acquire) == 0) {
            return std::nullopt;
        }
        T value;
        if (m_queue.try_dequeue(value)) {
            (void)tryPrefetchSingleRecvValues();
            m_size.fetch_sub(1, std::memory_order_acq_rel);
            return value;
        }
        return std::nullopt;
    }

    /**
     * @brief 按默认批量大小非阻塞接收数据
     * @return 取到数据时返回一批元素；通道为空时返回 std::nullopt
     */
    std::optional<std::vector<T>> tryRecvBatch() {
        return tryRecvBatch(m_defaultBatchSize);
    }

    /**
     * @brief 按指定批量大小非阻塞接收数据
     * @param maxCount 单次最多返回的元素数
     * @return 取到数据时返回最多 `maxCount` 条元素；通道为空时返回 std::nullopt
     */
    std::optional<std::vector<T>> tryRecvBatch(size_t maxCount) {
        if (m_size.load(std::memory_order_acquire) == 0) {
            return std::nullopt;
        }
        std::vector<T> values;
        values.reserve(maxCount);

        while (values.size() < maxCount) {
            auto cached = tryPopPrefetchedValue();
            if (!cached.has_value()) {
                break;
            }
            values.push_back(std::move(*cached));
        }

        if (values.size() < maxCount) {
            const size_t base = values.size();
            values.resize(maxCount);
            size_t count = m_queue.try_dequeue_bulk(values.data() + base, maxCount - base);
            if (count > 0) {
                m_size.fetch_sub(static_cast<uint32_t>(count), std::memory_order_acq_rel);
                values.resize(base + count);
            } else {
                values.resize(base);
            }
        }

        if (values.empty()) {
            return std::nullopt;
        }
        return values;
    }

    /**
     * @brief 获取当前待消费元素数量
     * @return 当前通道中的元素数近似值
     */
    size_t size() const {
        return m_size.load(std::memory_order_relaxed);
    }

    /**
     * @brief 检查通道是否为空
     * @return true 当前通道中没有待消费元素
     */
    bool empty() const {
        return m_size.load(std::memory_order_acquire) == 0;
    }

private:
    template <typename U>
    friend class MpscRecvAwaitable;
    template <typename U>
    friend class MpscRecvBatchAwaitable;
    friend struct MpscChannelTestAccess;

    bool hasWaiter() const noexcept {
        return m_waiter_registration.hasWaiter();
    }

    void publishWaiter(TaskState* waiterState) noexcept {
        (void)m_waiter_registration.arm(static_cast<void*>(waiterState));
    }

    bool clearWaiter(TaskState* waiterState) noexcept {
        return m_waiter_registration.clear(static_cast<void*>(waiterState));
    }

    void wakePublishedWaiter() noexcept {
        auto* waiterState = static_cast<TaskState*>(m_waiter_registration.consumeWake());
        if (!waiterState) {
            return;
        }
        Waker(TaskRef(waiterState, true)).wakeUp();
    }

    size_t prefetchedCount() const noexcept {
        return m_prefetchCount - m_prefetchIndex;
    }

    size_t singleRecvPrefetchLimit() const noexcept {
        return m_singleRecvPrefetchLimit;
    }

    std::optional<T> tryPopPrefetchedValue() {
        if (prefetchedCount() == 0) {
            return std::nullopt;
        }

        T value = std::move(m_singleRecvPrefetch[m_prefetchIndex]);
        ++m_prefetchIndex;
        if (m_prefetchIndex == m_prefetchCount) {
            m_prefetchIndex = 0;
            m_prefetchCount = 0;
        }
        m_size.fetch_sub(1, std::memory_order_acq_rel);
        return value;
    }

    bool tryPrefetchSingleRecvValues() {
        if (prefetchedCount() != 0 || m_singleRecvPrefetchLimit == 0) {
            return true;
        }

        const size_t count =
            m_queue.try_dequeue_bulk(m_singleRecvPrefetch.data(), m_singleRecvPrefetchLimit);
        if (count == 0) {
            return false;
        }

        m_prefetchIndex = 0;
        m_prefetchCount = count;
        return true;
    }


    alignas(64) std::atomic<uint32_t> m_size{0};
    WaitRegistration m_waiter_registration;
    moodycamel::ConcurrentQueue<T> m_queue;
    size_t m_defaultBatchSize{1024};
    size_t m_singleRecvPrefetchLimit{16};
    std::vector<T> m_singleRecvPrefetch;
    size_t m_prefetchIndex{0};
    size_t m_prefetchCount{0};
};

// ============================================================================
// MpscRecvAwaitable 实现
// ============================================================================

template <typename T>
inline bool MpscRecvAwaitable<T>::tryReceiveNow() noexcept {
    if (m_readyValue.has_value()) {
        return true;
    }
    if (auto value = m_channel->tryRecv(); value.has_value()) {
        m_readyValue = std::move(*value);
        return true;
    }
    return false;
}

template <typename T>
inline bool MpscRecvAwaitable<T>::await_ready() noexcept {
    return tryReceiveNow();
}

template <typename T>
template <typename Promise>
inline bool MpscRecvAwaitable<T>::await_suspend(
    std::coroutine_handle<Promise> handle) noexcept {
    if (tryReceiveNow()) {
        return false;
    }

    m_waiterState = handle.promise().taskRefView().state();
    m_channel->publishWaiter(m_waiterState);

    if (!tryReceiveNow()) {
        return true;
    }

    if (m_channel->clearWaiter(m_waiterState)) {
        return false;
    }

    return true;
}

template <typename T>
inline std::expected<T, IOError> MpscRecvAwaitable<T>::await_resume() noexcept {
    if (m_waiterState != nullptr) {
        TaskState* waiterState = m_waiterState;
        m_channel->clearWaiter(waiterState);
        m_waiterState = nullptr;
    }

    if (m_readyValue.has_value()) {
        return std::move(*m_readyValue);
    }

    if (auto value = m_channel->tryRecv(); value.has_value()) {
        return std::move(*value);
    }

    return std::unexpected(IOError(kTimeout, 0));
}

// ============================================================================
// MpscRecvBatchAwaitable 实现
// ============================================================================

template <typename T>
inline bool MpscRecvBatchAwaitable<T>::tryReceiveNow() noexcept {
    if (m_readyValues.has_value()) {
        return true;
    }
    if (auto values = m_channel->tryRecvBatch(m_maxCount); values.has_value()) {
        m_readyValues = std::move(*values);
        return true;
    }
    return false;
}

template <typename T>
inline bool MpscRecvBatchAwaitable<T>::await_ready() noexcept {
    return tryReceiveNow();
}

template <typename T>
template <typename Promise>
inline bool MpscRecvBatchAwaitable<T>::await_suspend(
    std::coroutine_handle<Promise> handle) noexcept {
    if (tryReceiveNow()) {
        return false;
    }

    m_waiterState = handle.promise().taskRefView().state();
    m_channel->publishWaiter(m_waiterState);

    if (!tryReceiveNow()) {
        return true;
    }

    if (m_channel->clearWaiter(m_waiterState)) {
        return false;
    }

    return true;
}

template <typename T>
inline std::expected<std::vector<T>, IOError> MpscRecvBatchAwaitable<T>::await_resume() noexcept {
    if (m_waiterState != nullptr) {
        TaskState* waiterState = m_waiterState;
        m_channel->clearWaiter(waiterState);
        m_waiterState = nullptr;
    }

    if (m_readyValues.has_value()) {
        return std::move(*m_readyValues);
    }

    if (auto values = m_channel->tryRecvBatch(m_maxCount); values.has_value()) {
        return std::move(*values);
    }
    return std::unexpected(IOError(kTimeout, 0));
}

} // namespace galay::kernel

#endif // GALAY_KERNEL_MPSC_CHANNEL_H
