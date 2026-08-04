/**
 * @file t53_prefetch.cc
 * @brief 用途：验证 `galay::mpsc::UnboundedChannel` 单条接收路径的预取优化不会破坏语义。
 * 关键覆盖点：单次接收预取、backlog 消耗、预取上限与最终一致性。
 * 通过条件：预取优化下消息仍完整可见，测试返回 0。
 */

#include <galay/cpp/galay-kernel/concurrency/mpsc/unbounded_channel.h>
#include "test/cpp/common/mpsc_access.h"

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

using namespace galay::kernel;

namespace {

struct ConcurrentPrefetchResult
{
    uint64_t received = 0;
    uint64_t checksum = 0;
    size_t pending = 0;
    size_t prefetched = 0;
    uint64_t emptyRetries = 0;
    bool tokenValid = false;
    bool sendOk = false;
    bool fifoOk = true;
    bool deadlineExpired = false;
    bool producerDoneAtDeadline = false;
};

ConcurrentPrefetchResult runConcurrentPrefetchCase(size_t prefetchLimit)
{
    using Channel = galay::mpsc::UnboundedChannel<uint64_t>;
    constexpr uint64_t kMessages = 5'000'000;
    constexpr auto kOverallDeadline = std::chrono::seconds(5);
    constexpr auto kDoneEmptyDeadline = std::chrono::milliseconds(200);

    Channel channel(Channel::DEFAULT_BATCH_SIZE, prefetchLimit);
    auto token = channel.makeProducerToken();
    if (!token.valid()) {
        return {};
    }

    std::barrier start(2);
    std::atomic<bool> producerDone{false};
    std::atomic<bool> sendFailed{false};
    ConcurrentPrefetchResult result;
    result.tokenValid = true;

    std::thread producer([&]() {
        start.arrive_and_wait();
        for (uint64_t sequence = 0; sequence < kMessages; ++sequence) {
            uint64_t value = sequence;
            if (!channel.send(token, std::move(value))) {
                sendFailed.store(true, std::memory_order_release);
                break;
            }
        }
        producerDone.store(true, std::memory_order_release);
    });

    std::thread consumer([&]() {
        start.arrive_and_wait();
        const auto overallDeadline =
            std::chrono::steady_clock::now() + kOverallDeadline;
        auto doneEmptyDeadline = overallDeadline;
        bool waitingAfterProducerDone = false;
        while (result.received < kMessages &&
               !sendFailed.load(std::memory_order_acquire)) {
            auto value = channel.tryRecv();
            if (value.has_value()) {
                waitingAfterProducerDone = false;
                if (*value != result.received) {
                    result.fifoOk = false;
                    break;
                }
                result.checksum += *value;
                ++result.received;
                continue;
            }

            ++result.emptyRetries;
            const auto now = std::chrono::steady_clock::now();
            const bool done = producerDone.load(std::memory_order_acquire);
            if (done && !waitingAfterProducerDone) {
                waitingAfterProducerDone = true;
                doneEmptyDeadline = now + kDoneEmptyDeadline;
            }
            if (now >= overallDeadline ||
                (waitingAfterProducerDone && now >= doneEmptyDeadline)) {
                result.deadlineExpired = true;
                result.producerDoneAtDeadline = done;
                break;
            }
            std::this_thread::yield();
        }
    });

    producer.join();
    consumer.join();
    result.sendOk = !sendFailed.load(std::memory_order_acquire);
    result.pending = channel.size();
    result.prefetched =
        galay::mpsc::UnboundedChannelTestAccess::prefetchedCount(channel);
    return result;
}

bool testConcurrentPrefetchAcrossRecycledBlocks()
{
    constexpr uint64_t kMessages = 5'000'000;
    constexpr uint64_t kExpectedChecksum =
        (kMessages - 1) * kMessages / 2;
    constexpr size_t kRepetitions = 8;
    constexpr std::array<size_t, 3> kPrefetchLimits{1, 4, 16};

    for (const size_t prefetchLimit : kPrefetchLimits) {
        for (size_t repetition = 0; repetition < kRepetitions; ++repetition) {
            const ConcurrentPrefetchResult result =
                runConcurrentPrefetchCase(prefetchLimit);
            if (!result.tokenValid || !result.sendOk || !result.fifoOk ||
                result.deadlineExpired || result.received != kMessages ||
                result.checksum != kExpectedChecksum || result.pending != 0 ||
                result.prefetched != 0) {
                std::cerr
                    << "[T53] concurrent prefetch stress failed: prefetch="
                    << prefetchLimit << " repetition=" << repetition
                    << " producer_done_at_deadline="
                    << result.producerDoneAtDeadline
                    << " deadline_expired=" << result.deadlineExpired
                    << " received=" << result.received
                    << " expected=" << kMessages
                    << " pending=" << result.pending
                  << " prefetched=" << result.prefetched
                  << " empty_retries=" << result.emptyRetries
                    << " fifo_ok=" << result.fifoOk
                    << " checksum=" << result.checksum
                    << " expected_checksum=" << kExpectedChecksum << '\n';
                return false;
            }
        }
    }
    return true;
}

} // namespace

int main() {
    constexpr int kMessageCount = 10;
    constexpr size_t kDefaultBatchSize = 4;
    constexpr size_t kPrefetchLimit = 3;

    galay::mpsc::UnboundedChannel<int> channel(kDefaultBatchSize, kPrefetchLimit);
    for (int i = 0; i < kMessageCount; ++i) {
        if (!channel.send(i)) {
            std::cerr << "[T53] failed to preload message " << i << "\n";
            return 1;
        }
    }

    auto first = channel.tryRecv();
    if (!first || *first != 0) {
        std::cerr << "[T53] expected first value 0\n";
        return 1;
    }

    const size_t prefetched = galay::mpsc::UnboundedChannelTestAccess::prefetchedCount(channel);
    if (prefetched == 0) {
        std::cerr << "[T53] expected single recv prefetch buffer to be populated\n";
        return 1;
    }
    if (prefetched > kPrefetchLimit) {
        std::cerr << "[T53] prefetch buffer exceeded configured limit\n";
        return 1;
    }

    if (channel.size() != static_cast<size_t>(kMessageCount - 1)) {
        std::cerr << "[T53] expected size to stay at " << (kMessageCount - 1)
                  << ", got " << channel.size() << "\n";
        return 1;
    }

    auto firstBatch = channel.tryRecvBatch();
    if (!firstBatch || firstBatch->size() != kDefaultBatchSize) {
        std::cerr << "[T53] expected no-arg tryRecvBatch to use configured batch size\n";
        return 1;
    }

    for (size_t i = 0; i < kDefaultBatchSize; ++i) {
        if ((*firstBatch)[i] != static_cast<int>(i + 1)) {
            std::cerr << "[T53] ordering mismatch at index " << (i - 1) << "\n";
            return 1;
        }
    }

    auto rest = channel.tryRecvBatch(kMessageCount);
    if (!rest || rest->size() != static_cast<size_t>(kMessageCount - 1 - kDefaultBatchSize)) {
        std::cerr << "[T53] expected second batch to drain remaining messages\n";
        return 1;
    }

    for (size_t i = 0; i < rest->size(); ++i) {
        const int expected = static_cast<int>(kDefaultBatchSize + 1 + i);
        if ((*rest)[i] != expected) {
            std::cerr << "[T53] trailing ordering mismatch at index " << i << "\n";
            return 1;
        }
    }

    if (galay::mpsc::UnboundedChannelTestAccess::prefetchedCount(channel) != 0) {
        std::cerr << "[T53] expected prefetch buffer to be empty after full drain\n";
        return 1;
    }

    if (!channel.empty()) {
        std::cerr << "[T53] expected channel to be empty after draining all messages\n";
        return 1;
    }

    galay::mpsc::UnboundedChannel<int> multiStreamChannel(
        kDefaultBatchSize, kPrefetchLimit);
    auto firstProducer = multiStreamChannel.makeProducerToken();
    auto secondProducer = multiStreamChannel.makeProducerToken();
    if (!firstProducer.valid() || !secondProducer.valid() ||
        !multiStreamChannel.send(firstProducer, 101) ||
        !multiStreamChannel.send(secondProducer, 202)) {
        std::cerr << "[T53] failed to prepare multi-stream prefetch case\n";
        return 1;
    }

    auto multiFirst = multiStreamChannel.tryRecv();
    if (!multiFirst ||
        galay::mpsc::UnboundedChannelTestAccess::prefetchedCount(
            multiStreamChannel) != 0) {
        std::cerr << "[T53] multi-stream receive must not build a prefetch cache\n";
        return 1;
    }
    auto multiSecond = multiStreamChannel.tryRecv();
    if (!multiSecond || *multiFirst + *multiSecond != 303 ||
        !multiStreamChannel.empty()) {
        std::cerr << "[T53] multi-stream receive ordering or drain failed\n";
        return 1;
    }

    if (!testConcurrentPrefetchAcrossRecycledBlocks()) {
        return 1;
    }

    std::cout << "T53-MpscSingleRecvPrefetch PASS\n";
    return 0;
}
