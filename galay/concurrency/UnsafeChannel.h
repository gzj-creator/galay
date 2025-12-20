#ifndef GALAY_KERNEL_CONCURRENCY_UNSAFECHANNEL_H
#define GALAY_KERNEL_CONCURRENCY_UNSAFECHANNEL_H

#include <cstddef>
#include <queue>
#include "galay/kernel/coroutine/Result.hpp"
#include "galay/kernel/coroutine/Waker.h"


namespace galay::unsafe
{

    template <typename T>
    class AsyncChannel;

    namespace details {
        template <typename T>
        class ChannelResult {
        public:
            ChannelResult(AsyncChannel<T>& channel)
                : m_channel(channel) {}
            bool await_ready();
            bool await_suspend(std::coroutine_handle<> handle);
            std::optional<T> await_resume() const;
        private:
            AsyncChannel<T>& m_channel;
        };

        template <typename T>
        class BatchChannelResult {
        public:
            BatchChannelResult(AsyncChannel<T>& channel)
                : m_channel(channel) {}
            bool await_ready();
            bool await_suspend(std::coroutine_handle<> handle);
            std::optional<std::vector<T>> await_resume() const;
        private:
            AsyncChannel<T>& m_channel;
        };
    }

    // thread unsafe channel
    template <typename T>
    class AsyncChannel
    {
        template <typename U>
        friend class details::ChannelResult;
        template<typename U>
        friend class details::BatchChannelResult;
    public:
        bool send(T&& value);
        bool sendBatch(const std::vector<T>& values);
        details::ChannelResult<T> recv();
        details::BatchChannelResult<T> recvBatch();

        size_t size() {
            return m_queue.size();
        }
    private:
        Waker m_waker;
        std::queue<T> m_queue;
    };
}

#include "UnsafeChannel.inl"

#endif // GALAY_KERNEL_CONCURRENCY_UNSAFECHANNEL_H