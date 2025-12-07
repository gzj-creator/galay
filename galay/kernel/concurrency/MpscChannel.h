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

        template<typename T>
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

    template <typename T>
    class AsyncChannel
    {
        template<typename U>
        friend class details::ChannelEvent;
        template<typename U>
        friend class details::BatchChannelEvent;
    public:
        static constexpr size_t BATCH_SIZE = 1024;

        bool send(T value) {
            if(!m_queue.enqueue(value)) {
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

        AsyncResult<std::optional<T>> recv() {
            return {m_event};
        }

        AsyncResult<std::optional<std::vector<T>>> recvBatch() {
            return {m_batchEvent};
        }
    private:
        // 内存对齐优化：将频繁访问的 m_size 单独对齐到缓存行
        // 避免与其他成员共享缓存行导致的 false sharing
        alignas(64) std::atomic_uint32_t m_size{0};
        moodycamel::ConcurrentQueue<T> m_queue;
        Waker m_waker;  // 放在最后，减少对 m_size 的影响
        std::shared_ptr<details::ChannelEvent<T>> m_event = std::make_shared<details::ChannelEvent<T>>(*this);
        std::shared_ptr<details::BatchChannelEvent<T>> m_batchEvent = std::make_shared<details::BatchChannelEvent<T>>(*this);
    };
}

#include "MpscChannel.inl"

#endif // GALAY_KERNEL_CONCURRENCY_MPSCCHANNEL_H