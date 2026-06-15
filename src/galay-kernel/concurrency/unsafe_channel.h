/**
 * @file unsafe_channel.h
 * @brief 轻量级单调度器异步通道（非线程安全）
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 非线程安全的异步通道，针对调度器内部协程通信优化。
 * 使用 std::deque 存储，无锁开销。提供单条接收、批量接收和
 * 攒批（阈值）接收操作，均兼容 co_await 和 .timeout() 链式调用。
 *
 * 特性：
 * - 零锁设计，在单个调度器内实现最高性能
 * - 内联与延迟唤醒两种模式
 * - 批量发送与接收
 * - 攒批接收，可配置唤醒前累积阈值
 *
 * @warning 请勿跨线程或跨调度器使用。跨线程通信请使用 MpscChannel。
 */

#ifndef GALAY_KERNEL_UNSAFE_CHANNEL_H
#define GALAY_KERNEL_UNSAFE_CHANNEL_H

#include "galay-kernel/core/task.h"
#include "galay-kernel/core/timeout.hpp"
#include "galay-kernel/core/waker.h"
#include "galay-kernel/common/error.h"
#include <concepts>
#include <deque>
#include <optional>
#include <expected>
#include <vector>
#include <cassert>

namespace galay::kernel
{

/**
 * @brief 约束 UnsafeChannel 可接受的元素类型
 * @tparam T 元素类型；必须可移动
 */
template <typename T>
concept UnsafeChannelValue = std::movable<T>;

/**
 * @brief UnsafeChannel 的唤醒策略
 * @details `Inline` 会在发送侧直接恢复等待协程，`Deferred` 会通过 Waker 回到调度器排队恢复。
 */
enum class UnsafeChannelWakeMode {
    Inline,    ///< 发送侧内联恢复等待协程，延迟最低
    Deferred,  ///< 发送侧仅发布唤醒，等待协程由调度器稍后恢复
};

template <typename T>
class UnsafeChannel;

/**
 * @brief 单条接收的等待体
 * @tparam T 通道元素类型
 */
template <typename T>
class UnsafeRecvAwaitable : public TimeoutSupport<UnsafeRecvAwaitable<T>>
{
public:
    static_assert(UnsafeChannelValue<T>, "UnsafeRecvAwaitable requires movable T");

    /**
     * @brief 构造单条接收等待体
     * @param channel 关联的通道对象；等待完成前必须保持有效
     */
    explicit UnsafeRecvAwaitable(UnsafeChannel<T>* channel) : m_channel(channel) {}

    bool await_ready() const noexcept;  ///< 如果通道已有数据，则返回 true 避免挂起
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) noexcept;  ///< 在通道为空时注册等待协程
    std::expected<T, IOError> await_resume() noexcept;  ///< 返回单条消息；超时则返回 IOError(kTimeout, 0)

private:
    friend struct WithTimeout<UnsafeRecvAwaitable<T>>;
    UnsafeChannel<T>* m_channel;
    std::expected<T, IOError> m_result;
};

/**
 * @brief 批量接收的等待体
 * @tparam T 通道元素类型
 */
template <typename T>
class UnsafeRecvBatchAwaitable : public TimeoutSupport<UnsafeRecvBatchAwaitable<T>>
{
public:
    static_assert(UnsafeChannelValue<T>, "UnsafeRecvBatchAwaitable requires movable T");

    /**
     * @brief 构造批量接收等待体
     * @param channel 关联的通道对象；等待完成前必须保持有效
     * @param maxCount 单次恢复时最多返回的元素数
     */
    explicit UnsafeRecvBatchAwaitable(UnsafeChannel<T>* channel, size_t maxCount)
        : m_channel(channel), m_maxCount(maxCount) {}

    bool await_ready() const noexcept;  ///< 如果通道已有数据，则返回 true 避免挂起
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) noexcept;  ///< 在通道为空时注册等待协程
    std::expected<std::vector<T>, IOError> await_resume() noexcept;  ///< 返回最多 m_maxCount 条消息；超时则返回 IOError(kTimeout, 0)

private:
    friend struct WithTimeout<UnsafeRecvBatchAwaitable<T>>;
    UnsafeChannel<T>* m_channel;
    size_t m_maxCount;
    std::expected<std::vector<T>, IOError> m_result;
};

/**
 * @brief 攒批接收的等待体
 *
 * 等待队列积累到 limit 条数据才唤醒，配合 .timeout() 可实现"达到 limit 或超时"的攒批语义。
 * 超时时若队列中有部分数据（< limit），返回已有数据而非错误。
 */
template <typename T>
class UnsafeRecvBatchedAwaitable : public TimeoutSupport<UnsafeRecvBatchedAwaitable<T>>
{
public:
    static_assert(UnsafeChannelValue<T>, "UnsafeRecvBatchedAwaitable requires movable T");

    /**
     * @brief 构造攒批接收等待体
     * @param channel 关联的通道对象；等待完成前必须保持有效
     * @param limit 唤醒前至少需要累积的元素数
     */
    explicit UnsafeRecvBatchedAwaitable(UnsafeChannel<T>* channel, size_t limit)
        : m_channel(channel), m_limit(limit) {}

    bool await_ready() const noexcept;  ///< 如果当前已达到攒批阈值，则返回 true 避免挂起
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) noexcept;  ///< 在未达到阈值时注册等待协程
    std::expected<std::vector<T>, IOError> await_resume() noexcept;  ///< 返回累计数据；纯超时且无数据时返回 IOError(kTimeout, 0)

private:
    friend struct WithTimeout<UnsafeRecvBatchedAwaitable<T>>;
    UnsafeChannel<T>* m_channel;
    size_t m_limit;
    std::expected<std::vector<T>, IOError> m_result;
};

/**
 * @brief 单调度器内部使用的轻量级异步通道
 *
 * 特点：
 * - 非线程安全，仅供同一调度器内的协程使用
 * - 无锁设计，性能优于 MpscChannel
 * - 使用 std::deque 作为底层容器，支持高效的头尾操作
 * - 适用于同一调度器内的生产者-消费者模式
 *
 * 使用场景：
 * - 同一调度器内的协程间通信
 * - 不需要跨线程的场景
 * - 对性能要求较高的场景
 *
 * 警告：
 * - 不要在不同调度器的协程间使用此通道
 * - 不要在普通线程中调用 send（除非该线程就是调度器线程）
 */
template <typename T>
class UnsafeChannel
{
public:
    static_assert(UnsafeChannelValue<T>, "UnsafeChannel requires movable T");
    static constexpr size_t DEFAULT_BATCH_SIZE = 1024;  ///< 批量接收默认上限

    /**
     * @brief 构造单调度器通道
     * @param wake_mode 数据到达时唤醒等待协程的策略
     */
    explicit UnsafeChannel(UnsafeChannelWakeMode wake_mode = UnsafeChannelWakeMode::Inline)
        : m_wake_mode(wake_mode)
    {
    }

    UnsafeChannel(const UnsafeChannel&) = delete;
    UnsafeChannel& operator=(const UnsafeChannel&) = delete;
    UnsafeChannel(UnsafeChannel&&) = delete;
    UnsafeChannel& operator=(UnsafeChannel&&) = delete;

    /**
     * @brief 发送单条数据
     * @param value 要发送的数据
     * @param immediately 是否立即唤醒 waiter，true=立即唤醒，false=等待达到阈值
     * @return true 数据已进入通道
     * @warning 仅在调度器线程内调用
     */
    bool send(T&& value, bool immediately = false) {
        m_queue.push_back(std::forward<T>(value));
        ++m_size;
        if (m_hasWaiter && (immediately || m_size >= m_wakeThreshold)) {
            wakeUpWaiter();
        }
        return true;
    }

    /**
     * @brief 复制发送单条数据
     * @param value 要复制进通道的数据
     * @param immediately 是否立即唤醒 waiter，true=立即唤醒，false=等待达到阈值
     * @return true 数据已进入通道
     * @warning 仅在调度器线程内调用
     */
    bool send(const T& value, bool immediately = false) requires std::copy_constructible<T> {
        m_queue.push_back(value);
        ++m_size;
        if (m_hasWaiter && (immediately || m_size >= m_wakeThreshold)) {
            wakeUpWaiter();
        }
        return true;
    }

    /**
     * @brief 批量发送数据
     * @param values 要发送的数据批次
     * @param immediately 是否立即唤醒 waiter，true=立即唤醒，false=等待达到阈值
     * @return true 批次数据已全部进入通道
     * @warning 仅在调度器线程内调用
     */
    bool sendBatch(const std::vector<T>& values, bool immediately = false) requires std::copy_constructible<T> {
        if (values.empty()) return true;
        for (const auto& value : values) {
            m_queue.push_back(value);
        }
        m_size += values.size();
        if (m_hasWaiter && (immediately || m_size >= m_wakeThreshold)) {
            wakeUpWaiter();
        }
        return true;
    }

    /**
     * @brief 移动发送一批数据
     * @param values 要移动进通道的数据批次
     * @param immediately 是否立即唤醒 waiter，true=立即唤醒，false=等待达到阈值
     * @return true 批次数据已全部进入通道
     * @warning 仅在调度器线程内调用
     */
    bool sendBatch(std::vector<T>&& values, bool immediately = false) {
        if (values.empty()) return true;
        for (auto& value : values) {
            m_queue.push_back(std::move(value));
        }
        m_size += values.size();
        if (m_hasWaiter && (immediately || m_size >= m_wakeThreshold)) {
            wakeUpWaiter();
        }
        return true;
    }

    /**
     * @brief 异步接收单条数据
     * @return 可 `co_await` 的等待体；若叠加 `.timeout(...)`，超时后返回 IOError
     */
    UnsafeRecvAwaitable<T> recv() {
        return UnsafeRecvAwaitable<T>(this);
    }

    /**
     * @brief 异步批量接收数据
     * @param maxCount 单次最多取出的元素个数
     * @return 可 `co_await` 的等待体；恢复后返回最多 `maxCount` 条数据
     */
    UnsafeRecvBatchAwaitable<T> recvBatch(size_t maxCount = DEFAULT_BATCH_SIZE) {
        return UnsafeRecvBatchAwaitable<T>(this, maxCount);
    }

    /**
     * @brief 异步攒批接收数据
     *
     * 等待队列积累到 limit 条数据才唤醒消费者，配合 .timeout() 使用：
     *   co_await channel.recvBatched(100).timeout(std::chrono::milliseconds(50));
     * 超时时若队列中有部分数据，返回已有数据而非错误。
     * @param limit 唤醒前至少需要积累的消息数
     * @return 可 `co_await` 的等待体；恢复后返回当前队列中的全部数据
     */
    UnsafeRecvBatchedAwaitable<T> recvBatched(size_t limit) {
        return UnsafeRecvBatchedAwaitable<T>(this, limit);
    }

    /**
     * @brief 非阻塞接收单条数据
     * @return 取到数据时返回该值；通道为空时返回 std::nullopt
     */
    std::optional<T> tryRecv() {
        if (m_size == 0) {
            return std::nullopt;
        }
        T value = std::move(m_queue.front());
        m_queue.pop_front();
        --m_size;
        return value;
    }

    /**
     * @brief 非阻塞批量接收数据
     * @param maxCount 单次最多取出的元素个数
     * @return 取到数据时返回最多 `maxCount` 条元素；通道为空时返回 std::nullopt
     */
    std::optional<std::vector<T>> tryRecvBatch(size_t maxCount = DEFAULT_BATCH_SIZE) {
        if (m_size == 0) {
            return std::nullopt;
        }
        std::vector<T> values;
        size_t count = std::min(maxCount, m_size);
        values.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            values.push_back(std::move(m_queue.front()));
            m_queue.pop_front();
        }
        m_size -= count;
        return values;
    }

    /**
     * @brief 获取队列大小
     * @return 当前待接收元素数量
     */
    size_t size() const {
        return m_size;
    }

    /**
     * @brief 检查队列是否为空
     * @return true 当前通道内没有待消费元素
     */
    bool empty() const {
        return m_size == 0;
    }

private:
    template <typename U>
    friend class UnsafeRecvAwaitable;
    template <typename U>
    friend class UnsafeRecvBatchAwaitable;
    template <typename U>
    friend class UnsafeRecvBatchedAwaitable;

    void wakeUpWaiter() {
        if (m_hasWaiter) {
            m_hasWaiter = false;
            m_wakeThreshold = 1;
            auto handle = m_waiterHandle;
            m_waiterHandle = {};
            TaskRef waiterTask = std::move(m_waiterTask);
            if (handle) {
                if (m_wake_mode == UnsafeChannelWakeMode::Deferred && waiterTask.isValid()) {
                    Waker(std::move(waiterTask)).wakeUp();
                    return;
                }
                // 默认保持内联恢复，避免影响现有通道语义
                handle.resume();
            }
        }
    }

    std::deque<T> m_queue;
    size_t m_size = 0;
    size_t m_wakeThreshold = 1;
    bool m_hasWaiter = false;
    UnsafeChannelWakeMode m_wake_mode = UnsafeChannelWakeMode::Inline;
    std::coroutine_handle<> m_waiterHandle;
    TaskRef m_waiterTask;
};

// ============================================================================
// UnsafeRecvAwaitable 实现
// ============================================================================

template <typename T>
inline bool UnsafeRecvAwaitable<T>::await_ready() const noexcept {
    return m_channel->m_size > 0;
}

template <typename T>
template <typename Promise>
inline bool UnsafeRecvAwaitable<T>::await_suspend(
    std::coroutine_handle<Promise> handle) noexcept {
    // 再次检查，避免竞态
    if (m_channel->m_size > 0) {
        return false;
    }

    m_channel->m_waiterHandle = handle;
    m_channel->m_waiterTask = handle.promise().taskRefView();
    m_channel->m_hasWaiter = true;

    return true;
}

template <typename T>
inline std::expected<T, IOError> UnsafeRecvAwaitable<T>::await_resume() noexcept {
    m_channel->m_hasWaiter = false;
    m_channel->m_waiterHandle = {};
    m_channel->m_waiterTask = {};
    if (m_channel->m_size == 0) {
        return std::unexpected(IOError(kTimeout, 0));
    }
    T value = std::move(m_channel->m_queue.front());
    m_channel->m_queue.pop_front();
    --m_channel->m_size;
    return value;
}

// ============================================================================
// UnsafeRecvBatchAwaitable 实现
// ============================================================================

template <typename T>
inline bool UnsafeRecvBatchAwaitable<T>::await_ready() const noexcept {
    return m_channel->m_size > 0;
}

template <typename T>
template <typename Promise>
inline bool UnsafeRecvBatchAwaitable<T>::await_suspend(
    std::coroutine_handle<Promise> handle) noexcept {
    // 再次检查，避免竞态
    if (m_channel->m_size > 0) {
        return false;
    }

    m_channel->m_waiterHandle = handle;
    m_channel->m_waiterTask = handle.promise().taskRefView();
    m_channel->m_hasWaiter = true;

    return true;
}

template <typename T>
inline std::expected<std::vector<T>, IOError> UnsafeRecvBatchAwaitable<T>::await_resume() noexcept {
    m_channel->m_hasWaiter = false;
    m_channel->m_waiterHandle = {};
    m_channel->m_waiterTask = {};
    if (m_channel->m_size == 0) {
        return std::unexpected(IOError(kTimeout, 0));
    }

    std::vector<T> values;
    size_t count = std::min(m_maxCount, m_channel->m_size);
    values.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        values.push_back(std::move(m_channel->m_queue.front()));
        m_channel->m_queue.pop_front();
    }
    m_channel->m_size -= count;

    return values;
}

// ============================================================================
// UnsafeRecvBatchedAwaitable 实现
// ============================================================================

template <typename T>
inline bool UnsafeRecvBatchedAwaitable<T>::await_ready() const noexcept {
    return m_channel->m_size >= m_limit;
}

template <typename T>
template <typename Promise>
inline bool UnsafeRecvBatchedAwaitable<T>::await_suspend(
    std::coroutine_handle<Promise> handle) noexcept {
    // 再次检查，避免竞态
    if (m_channel->m_size >= m_limit) {
        return false;
    }

    m_channel->m_waiterHandle = handle;
    m_channel->m_waiterTask = handle.promise().taskRefView();
    m_channel->m_hasWaiter = true;
    m_channel->m_wakeThreshold = m_limit;

    return true;
}

template <typename T>
inline std::expected<std::vector<T>, IOError> UnsafeRecvBatchedAwaitable<T>::await_resume() noexcept {
    m_channel->m_wakeThreshold = 1;
    m_channel->m_hasWaiter = false;
    m_channel->m_waiterHandle = {};
    m_channel->m_waiterTask = {};

    // 如果 m_result 已经被 WithTimeout 设置为超时错误，直接返回
    if (!m_result.has_value()) {
        return m_result;
    }

    // 否则，从队列中取数据
    if (m_channel->m_size == 0) {
        m_result = std::unexpected(IOError(kTimeout, 0));
        return m_result;
    }

    std::vector<T> values;
    values.reserve(m_channel->m_size);
    while (m_channel->m_size > 0) {
        values.push_back(std::move(m_channel->m_queue.front()));
        m_channel->m_queue.pop_front();
        --m_channel->m_size;
    }
    m_result = std::move(values);
    return m_result;
}

} // namespace galay::kernel

#endif // GALAY_KERNEL_UNSAFE_CHANNEL_H
