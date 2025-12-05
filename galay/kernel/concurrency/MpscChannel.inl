#ifndef GALAY_KERNEL_CONCURRENCY_MPSCCHANNEL_INL
#define GALAY_KERNEL_CONCURRENCY_MPSCCHANNEL_INL

#include "MpscChannel.h"

namespace galay::mpsc
{
    namespace details {
        inline bool ChannelEvent::onReady() {
            // Check if there are items available
            return m_channel.m_size.load(std::memory_order_acquire) > 0;
        }

        inline bool ChannelEvent::onSuspend(Waker waker) {
            // Store the waker first
            m_channel.m_waker = waker;
            if (m_channel.m_size.load(std::memory_order_acquire) > 0) {
                return false;  // Don't suspend, data is available
            }
            return true;
        }

        inline std::optional<int> ChannelEvent::onResume() {
            // Dequeue the value FIRST before resetting state
            int value;
            if (!m_channel.m_queue.try_dequeue(value)) {
                return std::nullopt;
            }
            m_channel.m_size.fetch_sub(1, std::memory_order_acq_rel);
            return value;
        }
    }
}
#endif