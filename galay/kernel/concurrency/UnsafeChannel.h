#ifndef GALAY_KERNEL_CONCURRENCY_UNSAFECHANNEL_H
#define GALAY_KERNEL_CONCURRENCY_UNSAFECHANNEL_H

#include <atomic>
#include <queue>
#include "galay/kernel/coroutine/Result.hpp"
#include "galay/kernel/coroutine/Waker.h"


namespace galay::unsafe
{

    template <typename T>
    class AsyncChannel;

    namespace details {
        template <typename T>
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

        template <typename T>
        class BatchChannelEvent : public AsyncEvent<std::optional<std::vector<T>>> {
        public:
            BatchChannelEvent(AsyncChannel<T>& channel)
                : m_channel(channel) {}
            bool onReady() override;
            bool onSuspend(Waker waker) override;
            std::optional<std::vector<T>> onResume() override;
        private:
            AsyncChannel<T>& m_channel;
        };
    }

    // thread unsafe channel
    template <typename T>
    class AsyncChannel
    {
        template <typename U>
        friend class details::ChannelEvent;
    public:
        bool send(T&& value);
        bool sendBatch(const std::vector<T>& values);
        AsyncResult<std::optional<T>> recv();
        AsyncResult<std::optional<std::vector<T>>> recvBatch();
    private:
        Waker m_waker;
        std::queue<T> m_queue;
    };
}

#include "UnsafeChannel.inl"

#endif // GALAY_KERNEL_CONCURRENCY_UNSAFECHANNEL_H