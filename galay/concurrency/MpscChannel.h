#ifndef GALAY_KERNEL_CONCURRENCY_MPSCCHANNEL_H
#define GALAY_KERNEL_CONCURRENCY_MPSCCHANNEL_H

#include "galay/kernel/coroutine/Result.hpp"
#include "galay/kernel/coroutine/Coroutine.hpp"
#include <atomic>
#include <concurrentqueue/moodycamel/concurrentqueue.h>
#include <cstdint>
#include <optional>

namespace galay::mpsc
{
    template <typename T>
    class AsyncChannel;

    namespace details {
        template<typename T>
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

        template<typename T>
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

    // thread safe mpsc channel
    template <typename T>
    class AsyncChannel
    {
        template<typename U>
        friend class details::ChannelResult;
        template<typename U>
        friend class details::BatchChannelResult;
    public:
        static constexpr size_t BATCH_SIZE = 1024;


        bool send(T&& value) {
            if(!m_queue.enqueue(std::move(value))) {
                return false;
            }
            uint32_t size = m_size.fetch_add(1, std::memory_order_acq_rel);
            if(size == 0) {
                m_waker.wakeUp();
            }
            return true;
        }

        bool sendBatch(const std::vector<T>& values) {
            if(!m_queue.enqueue_bulk(values.data(), values.size())) {
                return false;
            }
            uint32_t size = m_size.fetch_add(values.size(), std::memory_order_acq_rel);
            if(size == 0) {
                m_waker.wakeUp();
            }
            return true;
        }

        details::ChannelResult<T> recv() {
            return details::ChannelResult<T>(*this);
        }

        details::BatchChannelResult<T> recvBatch() {
            return details::BatchChannelResult<T>(*this);
        }
    private:
        // 内存对齐优化：将频繁访问的 m_size 单独对齐到缓存行
        // 避免与其他成员共享缓存行导致的 false sharing
        alignas(64) std::atomic_uint32_t m_size{0};
        moodycamel::ConcurrentQueue<T> m_queue;
        Waker m_waker;  // 放在最后，减少对 m_size 的影响
    };
}

#include "MpscChannel.inl"

#endif // GALAY_KERNEL_CONCURRENCY_MPSCCHANNEL_H