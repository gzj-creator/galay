#ifndef GALAY_KERNEL_CONCURRENCY_UNSAFECHANNEL_INL
#define GALAY_KERNEL_CONCURRENCY_UNSAFECHANNEL_INL

#include "UnsafeChannel.h"

namespace galay::unsafe
{
    namespace details {
        template <typename T>
        inline bool ChannelEvent<T>::onReady()
        {
            return !m_channel.m_queue.empty();
        }

        template <typename T>
        inline bool ChannelEvent<T>::onSuspend(Waker waker)
        {
            m_channel.m_waker = waker;
            return true;
        }

        template <typename T>
        inline std::optional<T> ChannelEvent<T>::onResume()
        {
            if(m_channel.m_queue.empty()) {
                return std::nullopt;
            }
            T value = m_channel.m_queue.front();
            m_channel.m_queue.pop();
            return value;
        }

        template <typename T>
        inline bool BatchChannelEvent<T>::onReady()
        {
            return !m_channel.m_queue.empty();
        }
        
        template <typename T>
        inline bool BatchChannelEvent<T>::onSuspend(Waker waker)
        {
            m_channel.m_waker = waker;
            return true;
        }

        template <typename T>
        inline std::optional<std::vector<T>> BatchChannelEvent<T>::onResume()
        {
            if(m_channel.m_queue.empty()) {
                return std::nullopt;
            }
            std::vector<T> values;
            size_t queue_size = m_channel.m_queue.size();
            values.reserve(queue_size);
            // 修复：应该使用 queue_size 而不是 values.size()（此时 values 还是空的）
            for(size_t i = 0; i < queue_size; ++i) {
                values.push_back(std::move(m_channel.m_queue.front()));
                m_channel.m_queue.pop();
            }
            values.shrink_to_fit();
            return values;
        }
    }

    template <typename T>
    inline bool AsyncChannel<T>::send(T value)
    {
        m_queue.push(value);
        if(m_queue.size() == 1) {
            m_waker.wakeUp();
        }
        return true;
    }
    
    template <typename T>
    inline bool AsyncChannel<T>::sendBatch(const std::vector<T>& values)
    {
        if(values.empty()) {
            return true;
        }
        // 检查发送前队列是否为空，如果为空则需要唤醒等待的协程
        bool was_empty = m_queue.empty();
        for(const auto& value : values) {
            m_queue.push(value);
        }
        // 如果之前队列为空，现在有数据了，需要唤醒等待的协程
        if(was_empty) {
            m_waker.wakeUp();
        }
        return true;
    }

    template <typename T>
    inline AsyncResult<std::optional<T>> AsyncChannel<T>::recv()
    {
        return AsyncResult<std::optional<T>>(std::make_shared<details::ChannelEvent<T>>(*this));
    }
    
    
    template <typename T>
    inline AsyncResult<std::optional<std::vector<T>>> AsyncChannel<T>::recvBatch()
    {
        return AsyncResult<std::optional<std::vector<T>>>(std::make_shared<details::BatchChannelEvent<T>>(*this));
    }
}

#endif // GALAY_KERNEL_CONCURRENCY_SPSCCHANNEL_INL