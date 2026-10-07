/**
 * @file t151_channel_namespaces.cc
 * @brief 验证 MPMC、MPSC、SPSC 有界与无界 channel 的公开分类和基本并发语义。
 */

#if __has_include("../../../src/cpp/galay-kernel/concurrency/bounded_channel.h")
#error "legacy concurrency/bounded_channel.h must not remain available"
#endif

#if __has_include("../../../src/cpp/galay-kernel/concurrency/mpsc_channel.h")
#error "legacy concurrency/mpsc_channel.h must not remain available"
#endif

#if __has_include("../../../src/cpp/galay-kernel/concurrency/unsafe_channel.h")
#error "legacy concurrency/unsafe_channel.h must not remain available"
#endif

#include <galay/cpp/galay-kernel/concurrency/mpmc/bounded_channel.h>
#include <galay/cpp/galay-kernel/concurrency/mpmc/unbounded_channel.h>
#include <galay/cpp/galay-kernel/concurrency/mpsc/bounded_channel.h>
#include <galay/cpp/galay-kernel/concurrency/mpsc/unbounded_channel.h>
#include <galay/cpp/galay-kernel/concurrency/spsc/bounded_channel.h>
#include <galay/cpp/galay-kernel/concurrency/spsc/unbounded_channel.h>
#include <galay/cpp/galay-kernel/parallel/parallel_scheduler.h>
#include <galay/cpp/galay-kernel/core/task.h>
#include "result_writer.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace galay::mpmc {

struct UnboundedChannelTestAccess {
    template <UnboundedValue T>
    static consteval size_t block_alignment()
    {
        return alignof(typename UnboundedChannel<T>::Block);
    }

    template <UnboundedValue T>
    static bool tail_uses_word_storage(const UnboundedChannel<T>& channel)
    {
        return std::is_same_v<
            std::remove_cvref_t<decltype(channel.m_tail)>,
            std::atomic<uint64_t>>;
    }

    template <UnboundedValue T>
    static bool flag_uses_byte_storage(const UnboundedChannel<T>& channel)
    {
        return std::is_same_v<
            std::remove_cvref_t<decltype(channel.m_recvWaiterPathUsed)>,
            std::atomic<uint8_t>>;
    }

    template <UnboundedValue T>
    static bool pump_uses_byte_storage(const UnboundedChannel<T>& channel)
    {
        return std::is_same_v<
            std::remove_cvref_t<decltype(channel.m_recvPumpState)>,
            std::atomic<uint8_t>>;
    }

    template <UnboundedValue T>
    static bool token_block_base_matches(
        const typename UnboundedChannel<T>::ProducerToken& token)
    {
        return token.m_block != nullptr &&
            token.m_block->base.load(std::memory_order_acquire) ==
                token.m_blockBase;
    }

    template <UnboundedValue T>
    static void register_recv_waiter_path(UnboundedChannel<T>& channel)
    {
        channel.register_recv_waiter_path();
    }

    template <UnboundedValue T>
    static bool waiter_path_used_after_publish(UnboundedChannel<T>& channel)
    {
        return channel.waiter_path_used_after_publish();
    }

    template <UnboundedValue T>
    static bool enqueue_waiter(
        UnboundedChannel<T>& channel,
        const std::shared_ptr<UnboundedChannelWaiter<T>>& waiter)
    {
        return channel.enqueue_waiter(waiter);
    }

    template <UnboundedValue T>
    static void request_recv_pump(UnboundedChannel<T>& channel)
    {
        channel.request_recv_pump();
    }

    template <UnboundedValue T>
    static bool recv_pump_idle(const UnboundedChannel<T>& channel)
    {
        return channel.m_recvPumpState.load(std::memory_order_acquire) == 0;
    }

    template <UnboundedValue T>
    static size_t drain_waiters(UnboundedChannel<T>& channel)
    {
        size_t count = 0;
        std::shared_ptr<UnboundedChannelWaiter<T>> waiter;
        while (channel.try_dequeue_waiter(waiter)) {
            ++count;
        }
        return count;
    }
};

} // namespace galay::mpmc

#if defined(__aarch64__) || defined(__ARM_ARCH_ISA_A64)
static_assert(galay::mpmc::UnboundedChannelTestAccess::block_alignment<int>() == 128);
#else
static_assert(galay::mpmc::UnboundedChannelTestAccess::block_alignment<int>() == 64);
#endif

namespace {

using namespace std::chrono_literals;

struct AsyncState {
    int value = 0;
    bool closed = false;
    bool timedOut = false;
    std::atomic<bool> entered{false};
    std::atomic<bool> done{false};
};

struct AsyncMultiState {
    std::atomic<int> started{0};
    std::atomic<int> done{0};
    std::atomic<int> received{0};
    std::atomic<bool> invalid{false};
};

bool wait_for(const std::atomic<bool>& flag)
{
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (flag.load(std::memory_order_acquire)) {
            return true;
        }
        std::this_thread::yield();
    }
    return flag.load(std::memory_order_acquire);
}

bool wait_for(const std::atomic<int>& value, int expected)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (value.load(std::memory_order_acquire) == expected) {
            return true;
        }
        std::this_thread::yield();
    }
    return value.load(std::memory_order_acquire) == expected;
}

galay::kernel::Task<void> receive_mpmc_unbounded(
    galay::mpmc::UnboundedChannel<int>* channel,
    AsyncState* state)
{
    state->entered.store(true, std::memory_order_release);
    auto result = co_await channel->recv();
    if (result.has_value()) {
        state->value = *result;
    } else {
        state->closed = galay::kernel::IOError::contains(result.error().code(),
                                                         galay::kernel::kClosed);
    }
    state->done.store(true, std::memory_order_release);
    co_return;
}

galay::kernel::Task<void> receive_mpmc_unbounded_with_timeout(
    galay::mpmc::UnboundedChannel<int>* channel,
    AsyncState* state)
{
    state->entered.store(true, std::memory_order_release);
    auto result = co_await channel->recv().timeout(2ms);
    state->timedOut = !result &&
        galay::kernel::IOError::contains(result.error().code(), galay::kernel::kTimeout);
    state->done.store(true, std::memory_order_release);
    co_return;
}

galay::kernel::Task<void> receive_many_mpmc_unbounded(
    galay::mpmc::UnboundedChannel<int>* channel,
    AsyncMultiState* state,
    std::atomic_uint8_t* seen,
    int count,
    int messageCount)
{
    state->started.fetch_add(1, std::memory_order_release);
    for (int i = 0; i < count; ++i) {
        auto result = co_await channel->recv();
        if (!result.has_value() || *result < 0 || *result >= messageCount) {
            state->invalid.store(true, std::memory_order_release);
            break;
        }
        seen[static_cast<size_t>(*result)].fetch_add(1, std::memory_order_relaxed);
        state->received.fetch_add(1, std::memory_order_release);
    }
    state->done.fetch_add(1, std::memory_order_release);
    co_return;
}

template <typename Channel>
bool check_bounded_channel()
{
    Channel channel(2);
    if (!channel.try_send(1) || !channel.try_send(2) || channel.try_send(3)) {
        return false;
    }
    auto first = channel.try_recv();
    auto second = channel.try_recv();
    return first.has_value() && second.has_value() && *first == 1 && *second == 2 &&
           !channel.try_recv().has_value();
}

template <typename Channel>
bool check_unbounded_channel()
{
    Channel channel;
    if (!channel.send(1) || !channel.send(2)) {
        return false;
    }
    auto first = channel.try_recv();
    auto second = channel.try_recv();
    return first.has_value() && second.has_value() && *first == 1 && *second == 2 &&
           !channel.try_recv().has_value();
}

bool check_mpmc_unbounded_concurrency()
{
    constexpr int kProducerCount = 4;
    constexpr int kConsumerCount = 4;
    constexpr int kMessagesPerProducer = 5'000;
    constexpr int kMessageCount = kProducerCount * kMessagesPerProducer;

    galay::mpmc::UnboundedChannel<int> channel;
    auto seen = std::make_unique<std::atomic_uint8_t[]>(kMessageCount);
    std::atomic<bool> sendFailed{false};
    std::atomic<int> producersDone{0};
    std::atomic<int> received{0};
    std::vector<std::thread> producers;
    std::vector<std::thread> consumers;

    for (int producer = 0; producer < kProducerCount; ++producer) {
        producers.emplace_back([&, producer]() {
            auto producerToken = channel.make_producer_token();
            if (!producerToken.valid()) {
                sendFailed.store(true, std::memory_order_release);
                producersDone.fetch_add(1, std::memory_order_release);
                return;
            }
            const int first = producer * kMessagesPerProducer;
            for (int offset = 0; offset < kMessagesPerProducer; ++offset) {
                if (!channel.send(producerToken, first + offset)) {
                    sendFailed.store(true, std::memory_order_release);
                    break;
                }
            }
            producersDone.fetch_add(1, std::memory_order_release);
        });
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (int consumer = 0; consumer < kConsumerCount; ++consumer) {
        consumers.emplace_back([&]() {
            auto consumerToken = channel.make_consumer_token();
            if (!consumerToken.valid()) {
                sendFailed.store(true, std::memory_order_release);
                return;
            }
            while (std::chrono::steady_clock::now() < deadline) {
                auto value = channel.try_recv(consumerToken);
                if (value.has_value()) {
                    if (*value < 0 || *value >= kMessageCount) {
                        sendFailed.store(true, std::memory_order_release);
                        return;
                    }
                    seen[static_cast<size_t>(*value)].fetch_add(1, std::memory_order_relaxed);
                    received.fetch_add(1, std::memory_order_release);
                    continue;
                }
                if (producersDone.load(std::memory_order_acquire) == kProducerCount &&
                    received.load(std::memory_order_acquire) == kMessageCount) {
                    return;
                }
                std::this_thread::yield();
            }
        });
    }

    for (auto& producer : producers) {
        producer.join();
    }
    for (auto& consumer : consumers) {
        consumer.join();
    }

    if (sendFailed.load(std::memory_order_acquire) ||
        received.load(std::memory_order_acquire) != kMessageCount) {
        return false;
    }
    for (int i = 0; i < kMessageCount; ++i) {
        if (seen[static_cast<size_t>(i)].load(std::memory_order_relaxed) != 1) {
            return false;
        }
    }
    return true;
}

bool check_mpmc_unbounded_token_surface()
{
    using Channel = galay::mpmc::UnboundedChannel<int>;
    static_assert(std::is_move_constructible_v<typename Channel::ProducerToken>);
    static_assert(std::is_move_assignable_v<typename Channel::ProducerToken>);
    static_assert(!std::is_copy_constructible_v<typename Channel::ProducerToken>);
    static_assert(!std::is_copy_assignable_v<typename Channel::ProducerToken>);
    static_assert(std::is_move_constructible_v<typename Channel::ConsumerToken>);
    static_assert(std::is_move_assignable_v<typename Channel::ConsumerToken>);
    static_assert(!std::is_copy_constructible_v<typename Channel::ConsumerToken>);
    static_assert(!std::is_copy_assignable_v<typename Channel::ConsumerToken>);

    Channel channel;
    auto producer = channel.make_producer_token();
    auto consumer = channel.make_consumer_token();
    if (!producer.valid() || !consumer.valid() || !channel.empty()) {
        return false;
    }
    if (!channel.send(producer, 11) || !channel.send(producer, 12) ||
        channel.size() != 2 || channel.empty()) {
        return false;
    }

    auto first = channel.try_recv(consumer);
    if (!first.has_value() || *first != 11 || channel.size() != 1) {
        return false;
    }

    auto movedProducer = std::move(producer);
    auto movedConsumer = std::move(consumer);
    if (producer.valid() || consumer.valid() || !movedProducer.valid() ||
        !movedConsumer.valid() || !channel.send(movedProducer, 13)) {
        return false;
    }

    Channel other;
    if (other.send(movedProducer, 99) ||
        other.try_recv(movedConsumer).has_value()) {
        return false;
    }

    auto remaining = channel.try_recv_batch(movedConsumer, 2);
    return remaining.has_value() && remaining->size() == 2 &&
        (*remaining)[0] == 12 && (*remaining)[1] == 13 && channel.empty() &&
        channel.size() == 0;
}

bool check_mpmc_unbounded_async_boundaries()
{
    galay::kernel::ParallelScheduler scheduler;
    auto started = scheduler.start();
    if (!started) {
        return false;
    }

    galay::mpmc::UnboundedChannel<int> wakeChannel;
    AsyncState wakeState;
    if (!galay::kernel::schedule_task(
            scheduler, receive_mpmc_unbounded(&wakeChannel, &wakeState)) ||
        !wait_for(wakeState.entered) || !wakeChannel.send(42) || !wait_for(wakeState.done) ||
        wakeState.value != 42) {
        scheduler.stop();
        return false;
    }

    galay::mpmc::UnboundedChannel<int> closeChannel;
    AsyncState closeState;
    if (!galay::kernel::schedule_task(
            scheduler, receive_mpmc_unbounded(&closeChannel, &closeState)) ||
        !wait_for(closeState.entered)) {
        scheduler.stop();
        return false;
    }
    closeChannel.close();
    if (!wait_for(closeState.done) || !closeState.closed) {
        scheduler.stop();
        return false;
    }
    std::vector<int> closedBatch{1, 2};
    if (closeChannel.send(1) || closeChannel.send_batch(std::move(closedBatch))) {
        scheduler.stop();
        return false;
    }

    galay::mpmc::UnboundedChannel<int> timeoutChannel;
    AsyncState timeoutState;
    if (!galay::kernel::schedule_task(
            scheduler, receive_mpmc_unbounded_with_timeout(&timeoutChannel, &timeoutState)) ||
        !wait_for(timeoutState.done) || !timeoutState.timedOut) {
        scheduler.stop();
        return false;
    }

    scheduler.stop();
    return true;
}

bool check_mpmc_unbounded_async_concurrency()
{
    constexpr int kProducerCount = 4;
    constexpr int kConsumerCount = 4;
    constexpr int kMessagesPerConsumer = 1'000;
    constexpr int kMessageCount = kConsumerCount * kMessagesPerConsumer;

    galay::kernel::ParallelScheduler scheduler;
    auto started = scheduler.start();
    if (!started) {
        return false;
    }

    galay::mpmc::UnboundedChannel<int> channel;
    AsyncMultiState state;
    auto seen = std::make_unique<std::atomic_uint8_t[]>(kMessageCount);
    for (int consumer = 0; consumer < kConsumerCount; ++consumer) {
        if (!galay::kernel::schedule_task(
                scheduler,
                receive_many_mpmc_unbounded(
                    &channel, &state, seen.get(), kMessagesPerConsumer, kMessageCount))) {
            channel.close();
            scheduler.stop();
            return false;
        }
    }
    if (!wait_for(state.started, kConsumerCount)) {
        channel.close();
        scheduler.stop();
        return false;
    }

    std::vector<std::thread> producers;
    const int messagesPerProducer = kMessageCount / kProducerCount;
    for (int producer = 0; producer < kProducerCount; ++producer) {
        producers.emplace_back([&, producer]() {
            const int first = producer * messagesPerProducer;
            for (int offset = 0; offset < messagesPerProducer; ++offset) {
                if (!channel.send(first + offset)) {
                    state.invalid.store(true, std::memory_order_release);
                    return;
                }
            }
        });
    }
    for (auto& producer : producers) {
        producer.join();
    }

    if (!wait_for(state.done, kConsumerCount)) {
        channel.close();
        if (!wait_for(state.done, kConsumerCount)) {
            scheduler.stop();
            return false;
        }
        scheduler.stop();
        return false;
    }
    scheduler.stop();

    if (state.invalid.load(std::memory_order_acquire) ||
        state.received.load(std::memory_order_acquire) != kMessageCount) {
        return false;
    }
    for (int i = 0; i < kMessageCount; ++i) {
        if (seen[static_cast<size_t>(i)].load(std::memory_order_relaxed) != 1) {
            return false;
        }
    }
    return true;
}

bool check_mpmc_unbounded_waiter_handshake()
{
    galay::mpmc::UnboundedChannel<int> publicationChannel;
    auto producerToken = publicationChannel.make_producer_token();
    if (!producerToken.valid()) {
        return false;
    }
    const bool tokenBaseBefore =
        galay::mpmc::UnboundedChannelTestAccess::token_block_base_matches<int>(
            producerToken);
    const bool defaultPublished = publicationChannel.send(1);
    const bool tokenPublished = publicationChannel.send(producerToken, 2);
    const bool tokenBaseAfter =
        galay::mpmc::UnboundedChannelTestAccess::token_block_base_matches<int>(
            producerToken);
    galay::mpmc::UnboundedChannel<int> handshakeChannel;
    const bool byteFlag =
        galay::mpmc::UnboundedChannelTestAccess::flag_uses_byte_storage(handshakeChannel);
    const bool bytePump =
        galay::mpmc::UnboundedChannelTestAccess::pump_uses_byte_storage(handshakeChannel);
    const bool wordTail =
        galay::mpmc::UnboundedChannelTestAccess::tail_uses_word_storage(
            handshakeChannel);
    const bool producerBeforeRegistration =
        galay::mpmc::UnboundedChannelTestAccess::waiter_path_used_after_publish(
            handshakeChannel);
    galay::mpmc::UnboundedChannelTestAccess::register_recv_waiter_path(
        handshakeChannel);
    const bool producerAfterRegistration =
        galay::mpmc::UnboundedChannelTestAccess::waiter_path_used_after_publish(
            handshakeChannel);

    galay::mpmc::UnboundedChannel<int> pumpChannel;
    auto waiter = std::make_shared<galay::mpmc::UnboundedChannelWaiter<int>>(
        galay::kernel::Waker());
    const bool enqueued =
        galay::mpmc::UnboundedChannelTestAccess::enqueue_waiter(pumpChannel, waiter);
    galay::mpmc::UnboundedChannelTestAccess::register_recv_waiter_path(pumpChannel);
    galay::mpmc::UnboundedChannelTestAccess::request_recv_pump(pumpChannel);
    const bool pumpIdle =
        galay::mpmc::UnboundedChannelTestAccess::recv_pump_idle(pumpChannel);
    const size_t queuedCopies =
        galay::mpmc::UnboundedChannelTestAccess::drain_waiters(pumpChannel);

    return tokenBaseBefore && defaultPublished && tokenPublished &&
        tokenBaseAfter && byteFlag && bytePump && wordTail &&
        !producerBeforeRegistration && producerAfterRegistration && enqueued &&
        pumpIdle &&
        waiter->state.load(std::memory_order_acquire) ==
            galay::mpmc::UnboundedWaiterState::kWaiting &&
        queuedCopies == 1;
}

bool check_mpmc_unbounded_queued_value_precedes_new_send()
{
    galay::mpmc::UnboundedChannel<int> channel;
    if (!channel.send(1)) {
        return false;
    }

    auto waiter = std::make_shared<galay::mpmc::UnboundedChannelWaiter<int>>(
        galay::kernel::Waker());
    if (!galay::mpmc::UnboundedChannelTestAccess::enqueue_waiter(channel, waiter)) {
        return false;
    }
    galay::mpmc::UnboundedChannelTestAccess::register_recv_waiter_path(channel);
    if (!channel.send(2)) {
        return false;
    }
    if (!waiter->value.has_value() || *waiter->value != 1) {
        return false;
    }
    auto remaining = channel.try_recv();
    return remaining.has_value() && *remaining == 2 && channel.empty();
}

} // namespace

int main()
{
    galay::test::TestResultWriter writer("t151_channel_namespaces");
    writer.add_test();

    static_assert(!std::is_same_v<galay::mpmc::BoundedChannel<int>,
                                  galay::mpsc::BoundedChannel<int>>);
    static_assert(!std::is_same_v<galay::mpmc::BoundedChannel<int>,
                                  galay::spsc::BoundedChannel<int>>);

    const bool ok =
        check_bounded_channel<galay::mpmc::BoundedChannel<int>>() &&
        check_unbounded_channel<galay::mpmc::UnboundedChannel<int>>() &&
        check_bounded_channel<galay::mpsc::BoundedChannel<int>>() &&
        check_unbounded_channel<galay::mpsc::UnboundedChannel<int>>() &&
        check_bounded_channel<galay::spsc::BoundedChannel<int>>() &&
        check_unbounded_channel<galay::spsc::UnboundedChannel<int>>() &&
        check_mpmc_unbounded_token_surface() &&
        check_mpmc_unbounded_concurrency() &&
        check_mpmc_unbounded_async_boundaries() &&
        check_mpmc_unbounded_async_concurrency() &&
        check_mpmc_unbounded_waiter_handshake() &&
        check_mpmc_unbounded_queued_value_precedes_new_send();

    if (!ok) {
        std::cerr << "channel namespace classification test failed\n";
        writer.add_failed();
        writer.write_result();
        return 1;
    }
    writer.add_passed();
    writer.write_result();
    std::cout << "t151_channel_namespaces PASS\n";
    return 0;
}
