#ifndef GALAY_KERNEL_CONCURRENCY_MPSCCHANNEL_H
#define GALAY_KERNEL_CONCURRENCY_MPSCCHANNEL_H

#include "galay/kernel/coroutine/Result.hpp"
#include <atomic>
#include <concurrentqueue/moodycamel/concurrentqueue.h>
#include <cstdint>
#include <memory>
#include <optional>

namespace galay::mpsc
{
    template <typename T>
    class AsyncChannel;

    namespace details {
        template<typename T>
        class ChannelEvent : public AsyncEvent<std::optional<T>> {
        public:
            ChannelEvent(AsyncChannel<T>& channel)
                : m_channel(channel) {}
            bool onReady() override;
            bool onSuspend(Waker waker) override;
            std::optional<T> onResume() override;
        private:
            AsyncChannel<T>& m_channel;
        };
    }

    template <typename T>
    class AsyncChannel
    {
        template<typename U>
        friend class details::ChannelEvent;
    public:
        void send(T value) {
            m_queue.enqueue(value);
            uint32_t size = m_size.fetch_add(1, std::memory_order_acq_rel);
            if(size == 0) {
                m_waker.wakeUp();
            }
        }

        AsyncResult<std::optional<T>> recv() {
            return {std::make_shared<details::ChannelEvent<T>>(*this)};
        }
    private:
        Waker m_waker;
        std::atomic_uint32_t m_size{0};
        moodycamel::ConcurrentQueue<T> m_queue;
    };
}

#include "MpscChannel.inl"

#endif // GALAY_KERNEL_CONCURRENCY_MPSCCHANNEL_H