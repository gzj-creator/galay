/**
 * @file channel_factory.h
 * @brief 统一的 MPSC 通道工厂 API
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 提供统一的工厂函数用于创建有界 MPSC 通道，支持根据策略选择底层实现：
 * - Latency: 延迟优先（使用指数退避的 BoundedChannel）
 * - Throughput: 吞吐优先（使用 per-producer ring 的 ThroughputBoundedChannel）
 * - Auto: 自动检测并发度并选择最优策略
 *
 * 环境变量：
 * - GALAY_MPSC_STRATEGY: "latency" | "throughput" | "auto" (覆盖默认策略)
 */

#ifndef GALAY_CONCURRENCY_MPSC_CHANNEL_FACTORY_H
#define GALAY_CONCURRENCY_MPSC_CHANNEL_FACTORY_H

#include "bounded_channel.h"
#include "throughput_bounded_channel.h"

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <string_view>
#include <thread>
#include <type_traits>
#include <variant>

namespace galay::mpsc
{

/**
 * @brief MPSC 通道策略枚举。
 */
enum class ChannelStrategy : uint8_t {
    /**
     * @brief 延迟优先策略。
     * @details 使用指数退避的 BoundedChannel，适用于：
     * - 低并发场景（≤4P）
     * - 对延迟敏感的应用
     * - 消息速率不均匀的场景
     */
    Latency,

    /**
     * @brief 吞吐优先策略。
     * @details 使用 per-producer ring 的 ThroughputBoundedChannel，适用于：
     * - 高并发场景（≥4P）
     * - 对吞吐量敏感的应用
     * - 持续高负载的场景
     */
    Throughput,

    /**
     * @brief 自动选择策略。
     * @details 根据检测到的并发度自动选择：
     * - CPU 核心数 ≤ 4 → Latency
     * - CPU 核心数 > 4 → Throughput
     * - 可通过环境变量 GALAY_MPSC_STRATEGY 覆盖
     */
    Auto
};

namespace channel_factory_detail
{

/**
 * @brief 从环境变量解析通道策略。
 * @return 解析成功返回对应策略；环境变量未设置或无效时返回 std::nullopt。
 */
inline std::optional<ChannelStrategy> parseStrategyFromEnv() noexcept
{
    const char* envValue = std::getenv("GALAY_MPSC_STRATEGY");
    if (envValue == nullptr) {
        return std::nullopt;
    }

    const std::string_view strategy(envValue);
    if (strategy == "latency" || strategy == "Latency") {
        return ChannelStrategy::Latency;
    }
    if (strategy == "throughput" || strategy == "Throughput") {
        return ChannelStrategy::Throughput;
    }
    if (strategy == "auto" || strategy == "Auto") {
        return ChannelStrategy::Auto;
    }

    return std::nullopt;
}

/**
 * @brief 检测系统并发度。
 * @return CPU 硬件线程数；检测失败时返回 1。
 */
inline size_t detectConcurrency() noexcept
{
    const unsigned int hwConcurrency = std::thread::hardware_concurrency();
    return hwConcurrency > 0 ? static_cast<size_t>(hwConcurrency) : 1;
}

/**
 * @brief 根据并发度自动选择策略。
 * @param concurrency CPU 核心数或预期生产者数。
 * @return 推荐的通道策略。
 */
inline ChannelStrategy selectAutoStrategy(size_t concurrency) noexcept
{
    // 并发度 ≤ 4 时，指数退避的开销可接受，优先选择延迟更低的 BoundedChannel
    // 并发度 > 4 时，per-producer ring 的吞吐优势明显，选择 ThroughputBoundedChannel
    return concurrency <= 4 ? ChannelStrategy::Latency
                            : ChannelStrategy::Throughput;
}

/**
 * @brief 解析最终生效的策略。
 * @param requestedStrategy 用户请求的策略。
 * @param maxProducers 预期最大生产者数（用于 Auto 策略的并发度估算）。
 * @return 最终生效的策略（已解析 Auto 和环境变量）。
 */
inline ChannelStrategy resolveStrategy(ChannelStrategy requestedStrategy,
                                       size_t maxProducers) noexcept
{
    // 1. 优先使用环境变量（便于运行时调优和 A/B 测试）
    if (auto envStrategy = parseStrategyFromEnv(); envStrategy.has_value()) {
        if (*envStrategy == ChannelStrategy::Auto) {
            const size_t concurrency =
                maxProducers > 0 ? maxProducers : detectConcurrency();
            return selectAutoStrategy(concurrency);
        }
        return *envStrategy;
    }

    // 2. 处理用户指定的 Auto 策略
    if (requestedStrategy == ChannelStrategy::Auto) {
        const size_t concurrency =
            maxProducers > 0 ? maxProducers : detectConcurrency();
        return selectAutoStrategy(concurrency);
    }

    // 3. 直接使用用户指定的策略
    return requestedStrategy;
}

} // namespace channel_factory_detail

/**
 * @brief 类型擦除的 MPSC 通道包装器。
 * @tparam T 通道元素类型。
 *
 * @details 使用 std::variant 封装不同策略的通道实现，提供统一的接口。
 * 通过访问者模式转发调用到底层实现，实现零开销抽象（内联后无虚函数开销）。
 */
template <typename T>
    requires BoundedValue<T> && ThroughputBoundedValue<T>
class UnifiedChannel
{
public:
    /**
     * @brief 从 Latency 策略通道构造。
     */
    explicit UnifiedChannel(std::unique_ptr<BoundedChannel<T>> channel)
        : m_impl(std::move(channel))
    {
    }

    /**
     * @brief 从 Throughput 策略通道构造。
     */
    explicit UnifiedChannel(std::unique_ptr<ThroughputBoundedChannel<T>> channel)
        : m_impl(std::move(channel))
    {
    }

    /**
     * @brief 尝试立即发送一条消息。
     * @param value 待发送消息；只有发送成功时才会被移动。
     * @return true 表示发送成功；false 表示通道已关闭或当前已满。
     */
    bool trySend(T&& value)
    {
        return std::visit(
            [&value](auto& channel) -> bool {
                return channel->trySend(std::move(value));
            },
            m_impl);
    }

    /**
     * @brief 复制并尝试立即发送一条消息。
     * @param value 待复制消息。
     * @return true 表示发送成功；false 表示通道已关闭或当前已满。
     */
    bool trySend(const T& value)
        requires std::copy_constructible<T>
    {
        return std::visit(
            [&value](auto& channel) -> bool {
                return channel->trySend(value);
            },
            m_impl);
    }

    /**
     * @brief 查询通道是否支持异步操作（send/recv/recvBatch）。
     * @return Latency 策略返回 true；Throughput 策略返回 false。
     */
    bool supportsAsync() const noexcept
    {
        return std::holds_alternative<std::unique_ptr<BoundedChannel<T>>>(m_impl);
    }

    /**
     * @brief 异步发送一条消息。
     * @param value 待发送消息；会移动进等待体。
     * @return 可 co_await 的等待体；关闭返回 IOError(kClosed)，超时返回 IOError(kTimeout)。
     * @note 满时挂起协程而不阻塞调度器线程。
     * @note 仅 Latency 策略支持；Throughput 策略调用会触发运行时异常。
     * @throws std::bad_variant_access 如果当前策略不支持异步操作。
     */
    auto send(T&& value)
    {
        return std::get<std::unique_ptr<BoundedChannel<T>>>(m_impl)->send(
            std::move(value));
    }

    /**
     * @brief 尝试立即接收一条消息。
     * @return 有消息时返回消息；为空时返回 std::nullopt。
     */
    std::optional<T> tryRecv()
    {
        return std::visit(
            [](auto& channel) -> std::optional<T> {
                return channel->tryRecv();
            },
            m_impl);
    }

    /**
     * @brief 异步接收一条消息。
     * @return 可 co_await 的等待体；关闭且已排空时返回 IOError(kClosed)，超时返回 IOError(kTimeout)。
     * @note 空时挂起协程而不阻塞调度器线程。
     * @note 仅 Latency 策略支持；Throughput 策略调用会触发运行时异常。
     * @throws std::bad_variant_access 如果当前策略不支持异步操作。
     */
    auto recv()
    {
        return std::get<std::unique_ptr<BoundedChannel<T>>>(m_impl)->recv();
    }

    /**
     * @brief 异步批量接收消息。
     * @param count 单次最多接收的消息数。
     * @return 可 co_await 的等待体；至少收到一条后尽量补齐至 count 条。
     * @note 仅 Latency 策略支持；Throughput 策略调用会触发运行时异常。
     * @throws std::bad_variant_access 如果当前策略不支持异步操作。
     */
    auto recvBatch(size_t count)
    {
        return std::get<std::unique_ptr<BoundedChannel<T>>>(m_impl)->recvBatch(
            count);
    }

    /**
     * @brief 关闭通道并唤醒所有等待者。
     * @note 操作幂等；关闭后发送失败，接收仍会先排空 ring 中的残留消息。
     */
    void close() noexcept
    {
        std::visit([](auto& channel) { channel->close(); }, m_impl);
    }

    /**
     * @brief 查询通道是否已关闭。
     * @return true 表示已关闭。
     */
    bool isClosed() const noexcept
    {
        return std::visit(
            [](const auto& channel) { return channel->isClosed(); },
            m_impl);
    }

    /**
     * @brief 返回实际生效容量。
     * @return 取整并钳制后的 2 的幂容量。
     */
    size_t capacity() const noexcept
    {
        return std::visit(
            [](const auto& channel) { return channel->capacity(); },
            m_impl);
    }

    /**
     * @brief 返回通道中的近似消息数。
     * @return 仅供诊断，不可用作同步条件。
     * @note 仅 Latency 策略支持；Throughput 策略返回 0。
     */
    size_t size() const noexcept
    {
        if (std::holds_alternative<std::unique_ptr<BoundedChannel<T>>>(m_impl)) {
            return std::get<std::unique_ptr<BoundedChannel<T>>>(m_impl)->size();
        }
        return 0; // ThroughputBoundedChannel 不提供 size()
    }

    /**
     * @brief 查询当前使用的策略。
     * @return Latency 或 Throughput。
     */
    ChannelStrategy strategy() const noexcept
    {
        return std::holds_alternative<std::unique_ptr<BoundedChannel<T>>>(m_impl)
                   ? ChannelStrategy::Latency
                   : ChannelStrategy::Throughput;
    }

private:
    std::variant<std::unique_ptr<BoundedChannel<T>>,
                 std::unique_ptr<ThroughputBoundedChannel<T>>>
        m_impl;
};

/**
 * @brief 创建有界 MPSC 通道。
 * @tparam T 元素类型，要求可移动且移动构造不得抛出异常。
 * @param capacity 通道容量（会向上取整为不小于 2 的 2 的幂）。
 * @param strategy 通道策略（Latency、Throughput 或 Auto，默认 Auto）。
 * @param maxProducers 预期最大生产者数（仅 Throughput 和 Auto 策略使用，默认 8）。
 * @return 统一接口的 MPSC 通道智能指针。
 *
 * @details
 * 策略选择规则：
 * 1. 环境变量 GALAY_MPSC_STRATEGY 优先（便于运行时调优）
 * 2. 用户指定的 strategy 参数
 * 3. Auto 策略自动选择：
 *    - maxProducers ≤ 4 → Latency（指数退避开销可接受）
 *    - maxProducers > 4 → Throughput（per-producer ring 优势明显）
 *
 * 容量分配（Throughput 策略）：
 * - 每个 producer ring 容量 = capacity / maxProducers（向上取整为 2 的幂）
 * - 最小 ring 容量 64，保证批量发布效率
 *
 * @note 类型约束：T 必须同时满足 BoundedValue 和 ThroughputBoundedValue。
 * @note 线程安全：多个生产者可并发调用 trySend，唯一消费者调用 tryRecv。
 * @note 向后兼容：现有代码可继续使用 BoundedChannel 和 ThroughputBoundedChannel。
 *
 * @example
 * ```cpp
 * // 自动选择策略
 * auto channel = makeBoundedChannel<int>(4096);
 *
 * // 显式指定延迟优先
 * auto latencyChannel = makeBoundedChannel<int>(4096, ChannelStrategy::Latency);
 *
 * // 显式指定吞吐优先，预期 16 个生产者
 * auto throughputChannel = makeBoundedChannel<int>(4096, ChannelStrategy::Throughput, 16);
 *
 * // 环境变量覆盖（运行时调优）
 * // export GALAY_MPSC_STRATEGY=throughput
 * auto adaptiveChannel = makeBoundedChannel<int>(4096, ChannelStrategy::Auto);
 * ```
 */
template <typename T>
    requires BoundedValue<T> && ThroughputBoundedValue<T>
inline UnifiedChannel<T> makeBoundedChannel(
    size_t capacity,
    ChannelStrategy strategy = ChannelStrategy::Auto,
    size_t maxProducers = 8)
{
    const ChannelStrategy finalStrategy =
        channel_factory_detail::resolveStrategy(strategy, maxProducers);

    if (finalStrategy == ChannelStrategy::Latency) {
        auto channel = std::make_unique<BoundedChannel<T>>(capacity);
        return UnifiedChannel<T>(std::move(channel));
    } else {
        auto channel = std::make_unique<ThroughputBoundedChannel<T>>(
            capacity, maxProducers);
        return UnifiedChannel<T>(std::move(channel));
    }
}

} // namespace galay::mpsc

#endif // GALAY_CONCURRENCY_MPSC_CHANNEL_FACTORY_H
