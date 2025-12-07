#ifndef GALAY_KERNEL_CONCURRENCY_MPSCCHANNEL_INL
#define GALAY_KERNEL_CONCURRENCY_MPSCCHANNEL_INL

#include "MpscChannel.h"

namespace galay::mpsc
{
    namespace details {
        template<typename T>
        inline bool ChannelEvent<T>::onReady() {
            return m_channel.m_size.load(std::memory_order_acquire) > 0;
        }

        template<typename T>
        inline bool ChannelEvent<T>::onSuspend(Waker waker) {
            m_channel.m_waker = waker;
            if (m_channel.m_size.load(std::memory_order_acquire) > 0) {
                return false;
            }
            return true;
        }

        template<typename T>
        inline std::optional<T> ChannelEvent<T>::onResume() {
            T value;
            if (!m_channel.m_queue.try_dequeue(value)) {
                return std::nullopt;
            }
            m_channel.m_size.fetch_sub(1, std::memory_order_acq_rel);
            return value;
        }

        template<typename T>
        inline bool BatchChannelEvent<T>::onReady() {
            return m_channel.m_size.load(std::memory_order_acquire) >= 0;
        }

        template<typename T>
        inline bool BatchChannelEvent<T>::onSuspend(Waker waker) {
            m_channel.m_waker = waker;
            if (m_channel.m_size.load(std::memory_order_acquire) > 0) {
                return false;
            }
            return true;
        }

        template<typename T>
        inline std::optional<std::vector<T>> BatchChannelEvent<T>::onResume() {
            std::vector<T> values(AsyncChannel<T>::BATCH_SIZE);
            size_t count = m_channel.m_queue.try_dequeue_bulk(values.data(), values.size());
            if (count == 0) {
                return std::nullopt;
            }
            m_channel.m_size.fetch_sub(count, std::memory_order_acq_rel);
            values.resize(count);
            return std::move(values);
        }
    }
}
#endif