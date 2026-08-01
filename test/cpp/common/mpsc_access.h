#ifndef GALAY_TEST_MPSC_CHANNEL_TEST_ACCESS_H
#define GALAY_TEST_MPSC_CHANNEL_TEST_ACCESS_H

#include <galay/cpp/galay-kernel/concurrency/mpsc/unbounded_channel.h>

#include <cstdint>
#include <type_traits>

namespace galay::mpsc {

struct UnboundedChannelTestAccess {
    template <typename T>
    static void holdProducerRegistration(UnboundedChannel<T>& channel) {
        channel.m_producerRegistrations.fetch_add(1, std::memory_order_seq_cst);
    }

    template <typename T>
    static void releaseProducerRegistration(UnboundedChannel<T>& channel) {
        channel.m_producerRegistrations.fetch_sub(1, std::memory_order_seq_cst);
    }

    template <typename T>
    static size_t allocatedStreamCount(const UnboundedChannel<T>& channel) {
        size_t count = 0;
        auto* stream = channel.m_streamHead.load(std::memory_order_acquire);
        while (stream != nullptr) {
            ++count;
            stream = stream->next;
        }
        return count;
    }

    template <typename T>
    static size_t allocatedBlockCount(const UnboundedChannel<T>& channel) {
        size_t count = 0;
        auto* stream = channel.m_streamHead.load(std::memory_order_acquire);
        while (stream != nullptr) {
            auto* block = stream->consumer.block;
            while (block != nullptr) {
                ++count;
                block = block->next.load(std::memory_order_relaxed);
            }
            block = stream->shared.recycledBlocks.load(std::memory_order_acquire);
            while (block != nullptr) {
                ++count;
                block = block->next.load(std::memory_order_relaxed);
            }
            stream = stream->next;
        }
        return count;
    }

    template <typename T>
    static size_t recycledBlockCount(const UnboundedChannel<T>& channel) {
        size_t count = 0;
        auto* stream = channel.m_streamHead.load(std::memory_order_acquire);
        while (stream != nullptr) {
            auto* block =
                stream->shared.recycledBlocks.load(std::memory_order_acquire);
            while (block != nullptr) {
                ++count;
                block = block->next.load(std::memory_order_relaxed);
            }
            stream = stream->next;
        }
        return count;
    }

    template <typename T>
    static size_t prefetchedCount(const UnboundedChannel<T>& channel) {
        return channel.prefetchedCount();
    }

    template <typename T>
    static bool seedOnlyStreamSequence(UnboundedChannel<T>& channel,
                                       uint64_t sequence) {
        auto* stream = channel.m_streamHead.load(std::memory_order_acquire);
        if (stream == nullptr || stream->next != nullptr) {
            return false;
        }
        stream->producer.localPublished = sequence;
        stream->consumer.localConsumed = sequence;
        stream->consumer.observedPublished = sequence;
        stream->consumer.consumed.store(sequence, std::memory_order_relaxed);
        stream->shared.published.store(sequence, std::memory_order_relaxed);
        return true;
    }

    template <typename T>
    static size_t setSyntheticPendingForAllStreams(
        UnboundedChannel<T>& channel, uint64_t pending) {
        size_t count = 0;
        auto* stream = channel.m_streamHead.load(std::memory_order_acquire);
        while (stream != nullptr) {
            stream->producer.localPublished = pending;
            stream->consumer.localConsumed = 0;
            stream->consumer.observedPublished = pending;
            stream->consumer.consumed.store(0, std::memory_order_relaxed);
            stream->shared.published.store(pending, std::memory_order_relaxed);
            ++count;
            stream = stream->next;
        }
        return count;
    }

    template <typename T>
    static bool setOnlyStreamObservedCounters(UnboundedChannel<T>& channel,
                                              uint64_t published,
                                              uint64_t consumed) {
        auto* stream = channel.m_streamHead.load(std::memory_order_acquire);
        if (stream == nullptr || stream->next != nullptr) {
            return false;
        }
        stream->shared.published.store(published, std::memory_order_relaxed);
        stream->consumer.consumed.store(consumed, std::memory_order_relaxed);
        return true;
    }

    template <typename T>
    static bool clearWaiter(UnboundedChannel<T>& channel, TaskState* waiter_state) {
        if constexpr (std::is_void_v<decltype(channel.clearWaiter(waiter_state))>) {
            channel.clearWaiter(waiter_state);
            return true;
        } else {
            return channel.clearWaiter(waiter_state);
        }
    }

    template <typename T>
    static bool publishWaiter(UnboundedChannel<T>& channel, TaskState* waiter_state) {
        return channel.publishWaiter(waiter_state);
    }

    template <typename T>
    static bool beginWaiterRegistration(UnboundedChannel<T>& channel) {
        return channel.beginWaiterRegistration();
    }

    template <typename T>
    static void cancelWaiterRegistration(UnboundedChannel<T>& channel) {
        channel.cancelWaiterRegistration();
    }
};

}  // namespace galay::mpsc

#endif
