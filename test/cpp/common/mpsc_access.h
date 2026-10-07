#ifndef GALAY_TEST_MPSC_CHANNEL_TEST_ACCESS_H
#define GALAY_TEST_MPSC_CHANNEL_TEST_ACCESS_H

#include <galay/cpp/galay-kernel/concurrency/mpsc/unbounded_channel.h>

#include <cstdint>
#include <type_traits>

namespace galay::mpsc {

struct UnboundedChannelTestAccess {
    template <typename T>
    static void hold_producer_registration(UnboundedChannel<T>& channel) {
        channel.m_producerRegistrations.fetch_add(1, std::memory_order_seq_cst);
    }

    template <typename T>
    static void release_producer_registration(UnboundedChannel<T>& channel) {
        channel.m_producerRegistrations.fetch_sub(1, std::memory_order_seq_cst);
    }

    template <typename T>
    static size_t allocated_stream_count(const UnboundedChannel<T>& channel) {
        size_t count = 0;
        auto* stream = channel.m_streamHead.load(std::memory_order_acquire);
        while (stream != nullptr) {
            ++count;
            stream = stream->next;
        }
        return count;
    }

    template <typename T>
    static size_t allocated_block_count(const UnboundedChannel<T>& channel) {
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
    static size_t recycled_block_count(const UnboundedChannel<T>& channel) {
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
    static size_t prefetched_count(const UnboundedChannel<T>& channel) {
        return channel.prefetched_count();
    }

    template <typename T>
    static bool seed_only_stream_sequence(UnboundedChannel<T>& channel,
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
    static size_t set_synthetic_pending_for_all_streams(
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
    static bool set_only_stream_observed_counters(UnboundedChannel<T>& channel,
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
    static bool clear_waiter(UnboundedChannel<T>& channel, TaskState* waiter_state) {
        if constexpr (std::is_void_v<decltype(channel.clear_waiter(waiter_state))>) {
            channel.clear_waiter(waiter_state);
            return true;
        } else {
            return channel.clear_waiter(waiter_state);
        }
    }

    template <typename T>
    static bool publish_waiter(UnboundedChannel<T>& channel, TaskState* waiter_state) {
        return channel.publish_waiter(waiter_state);
    }

    template <typename T>
    static bool begin_waiter_registration(UnboundedChannel<T>& channel) {
        return channel.begin_waiter_registration();
    }

    template <typename T>
    static void cancel_waiter_registration(UnboundedChannel<T>& channel) {
        channel.cancel_waiter_registration();
    }
};

}  // namespace galay::mpsc

#endif
