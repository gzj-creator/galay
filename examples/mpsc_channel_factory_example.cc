/**
 * @file mpsc_channel_factory_example.cc
 * @brief MPSC 通道工厂 API 使用示例
 *
 * 展示如何使用统一的 makeBoundedChannel 工厂函数创建和使用 MPSC 通道。
 */

#include "galay-kernel/concurrency/mpsc/channel_factory.h"
#include <cassert>
#include <iostream>
#include <thread>
#include <vector>

using namespace galay::mpsc;

// 示例 1: 自动策略选择
void example_auto_strategy()
{
    std::cout << "\n=== Example 1: Auto Strategy ===" << std::endl;

    // 自动根据系统并发度选择策略
    auto channel = makeBoundedChannel<int>(1024);

    std::cout << "Strategy selected: "
              << (channel.strategy() == ChannelStrategy::Latency ? "Latency"
                                                                  : "Throughput")
              << std::endl;
    std::cout << "Capacity: " << channel.capacity() << std::endl;
    std::cout << "Supports async: " << (channel.supportsAsync() ? "Yes" : "No")
              << std::endl;

    // 基本发送接收
    for (int i = 0; i < 10; ++i) {
        assert(channel.trySend(i));
    }

    for (int i = 0; i < 10; ++i) {
        auto value = channel.tryRecv();
        assert(value.has_value() && *value == i);
    }

    std::cout << "Auto strategy: OK" << std::endl;
}

// 示例 2: 延迟优先策略（支持异步）
void example_latency_strategy()
{
    std::cout << "\n=== Example 2: Latency Strategy ===" << std::endl;

    auto channel = makeBoundedChannel<int>(256, ChannelStrategy::Latency);

    assert(channel.strategy() == ChannelStrategy::Latency);
    assert(channel.supportsAsync());

    // 多生产者同步发送
    std::vector<std::thread> producers;
    constexpr int kProducers = 4;
    constexpr int kMessagesPerProducer = 100;

    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&channel, p]() {
            for (int i = 0; i < kMessagesPerProducer; ++i) {
                while (!channel.trySend(p * 1000 + i)) {
                    std::this_thread::yield();
                }
            }
        });
    }

    // 单消费者接收
    int received = 0;
    while (received < kProducers * kMessagesPerProducer) {
        if (auto value = channel.tryRecv(); value.has_value()) {
            ++received;
        }
    }

    for (auto& t : producers) {
        t.join();
    }

    std::cout << "Received " << received << " messages" << std::endl;
    std::cout << "Latency strategy: OK" << std::endl;
}

// 示例 3: 吞吐优先策略（高并发）
void example_throughput_strategy()
{
    std::cout << "\n=== Example 3: Throughput Strategy ===" << std::endl;

    constexpr int kProducers = 8;
    auto channel = makeBoundedChannel<int>(8192, ChannelStrategy::Throughput,
                                           kProducers);

    assert(channel.strategy() == ChannelStrategy::Throughput);
    assert(!channel.supportsAsync()); // Throughput 不支持异步

    // 高并发生产者
    std::vector<std::thread> producers;
    constexpr int kMessagesPerProducer = 10000;
    std::atomic<int> totalSent{0};

    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([&channel, &totalSent, p]() {
            int sent = 0;
            for (int i = 0; i < kMessagesPerProducer; ++i) {
                while (!channel.trySend(p * 100000 + i)) {
                    std::this_thread::yield();
                }
                ++sent;
            }
            totalSent.fetch_add(sent, std::memory_order_relaxed);
        });
    }

    // 单消费者
    std::thread consumer([&channel, expected = kProducers * kMessagesPerProducer]() {
        int received = 0;
        while (received < expected) {
            if (auto value = channel.tryRecv(); value.has_value()) {
                ++received;
            }
        }
        std::cout << "Consumer received " << received << " messages"
                  << std::endl;
    });

    for (auto& t : producers) {
        t.join();
    }
    consumer.join();

    std::cout << "Total sent: " << totalSent.load() << std::endl;
    std::cout << "Throughput strategy: OK" << std::endl;
}

// 示例 4: 环境变量覆盖
void example_env_override()
{
    std::cout << "\n=== Example 4: Environment Variable Override ==="
              << std::endl;

    // 检查环境变量
    const char* envStrategy = std::getenv("GALAY_MPSC_STRATEGY");
    if (envStrategy != nullptr) {
        std::cout << "GALAY_MPSC_STRATEGY=" << envStrategy << std::endl;
    } else {
        std::cout << "GALAY_MPSC_STRATEGY not set (using code default)"
                  << std::endl;
    }

    // 即使代码指定 Latency，环境变量也会覆盖
    auto channel = makeBoundedChannel<int>(1024, ChannelStrategy::Latency);

    std::cout << "Requested: Latency" << std::endl;
    std::cout << "Actual: "
              << (channel.strategy() == ChannelStrategy::Latency ? "Latency"
                                                                  : "Throughput")
              << std::endl;

    if (envStrategy != nullptr) {
        std::cout
            << "Note: Strategy was overridden by environment variable"
            << std::endl;
    }
}

// 示例 5: 通道关闭
void example_close()
{
    std::cout << "\n=== Example 5: Channel Close ===" << std::endl;

    auto channel = makeBoundedChannel<int>(64);

    // 发送一些消息
    for (int i = 0; i < 5; ++i) {
        channel.trySend(i);
    }

    // 关闭通道
    channel.close();
    assert(channel.isClosed());

    // 关闭后发送失败
    assert(!channel.trySend(999));

    // 关闭后仍可排空残留消息
    int count = 0;
    while (auto value = channel.tryRecv()) {
        ++count;
    }
    std::cout << "Drained " << count << " messages after close" << std::endl;

    std::cout << "Close behavior: OK" << std::endl;
}

int main()
{
    std::cout << "MPSC Channel Factory Examples" << std::endl;
    std::cout << "==============================" << std::endl;

    example_auto_strategy();
    example_latency_strategy();
    example_throughput_strategy();
    example_env_override();
    example_close();

    std::cout << "\n✓ All examples completed successfully!" << std::endl;
    return 0;
}
