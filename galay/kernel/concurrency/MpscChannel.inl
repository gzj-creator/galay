#ifndef GALAY_KERNEL_CONCURRENCY_MPSCCHANNEL_INL
#define GALAY_KERNEL_CONCURRENCY_MPSCCHANNEL_INL

#include "MpscChannel.h"

namespace galay::mpsc
{
    namespace details {
        template<typename T>
        inline bool ChannelEvent<T>::onReady() {
            // Check if there are items available
            return m_channel.m_size.load(std::memory_order_acquire) > 0;
        }

        template<typename T>
        inline bool ChannelEvent<T>::onSuspend(Waker waker) {
            // Store the waker first
            m_channel.m_waker = waker;
            if (m_channel.m_size.load(std::memory_order_acquire) > 0) {
                return false;  // Don't suspend, data is available
            }
            return true;
        }

        template<typename T>
        inline std::optional<T> ChannelEvent<T>::onResume() {
            // Dequeue the value FIRST before resetting state
            T value;
            if (!m_channel.m_queue.try_dequeue(value)) {
                return std::nullopt;
            }
            m_channel.m_size.fetch_sub(1, std::memory_order_acq_rel);
            return value;
        }
    }
}
#endif