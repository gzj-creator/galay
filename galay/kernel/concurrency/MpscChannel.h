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
    class AsyncChannel;

    namespace details {
        class ChannelEvent : public AsyncEvent<std::optional<int>> {
        public:
            ChannelEvent(AsyncChannel& channel)
                : m_channel(channel) {}
            bool onReady() override;
            bool onSuspend(Waker waker) override;
            std::optional<int> onResume() override;
        private:
            AsyncChannel& m_channel;
        };
    }

    class AsyncChannel
    {
        friend class details::ChannelEvent;
    public:
        void send(int value) {
            m_queue.enqueue(value);
            uint32_t size = m_size.fetch_add(1, std::memory_order_acq_rel);
            if(size == 0) {
                m_waker.wakeUp();
            }
        }

        AsyncResult<std::optional<int>> recv() {
            return {std::make_shared<details::ChannelEvent>(*this)};
        }
    private:
        Waker m_waker;
        std::atomic_uint32_t m_size{0};
        moodycamel::ConcurrentQueue<int> m_queue;
    };
}

#include "MpscChannel.inl"

#endif // GALAY_KERNEL_CONCURRENCY_MPSCCHANNEL_H