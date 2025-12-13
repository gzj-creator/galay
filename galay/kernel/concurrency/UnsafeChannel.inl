#ifndef GALAY_KERNEL_CONCURRENCY_UNSAFECHANNEL_INL
#define GALAY_KERNEL_CONCURRENCY_UNSAFECHANNEL_INL

#include "UnsafeChannel.h"

namespace galay::unsafe
{
    namespace details {
        template <typename T>
        inline bool ChannelResult<T>::await_ready()
        {
            return !m_channel.m_queue.empty();
        }

        template <typename T>
        inline bool ChannelResult<T>::await_suspend(std::coroutine_handle<> handle)
        {
            auto wait_co = std::coroutine_handle<PromiseTypeBase>::from_address(handle.address()).promise().getCoroutine();
            if(auto co_ptr = wait_co.lock()) {
                co_ptr->modToSuspend();
            }
            m_channel.m_waker = Waker(wait_co);
            return true;
        }

        template <typename T>
        inline std::optional<T> ChannelResult<T>::await_resume() const
        {
            auto co = m_channel.m_waker.getCoroutine();
            if(auto co_ptr = co.lock()) {
                co_ptr->modToRunning();
            }
            T value = std::move(m_channel.m_queue.front());
            m_channel.m_queue.pop();
            return std::move(value);
        }

        template <typename T>
        inline bool BatchChannelResult<T>::await_ready()
        {
            return !m_channel.m_queue.empty();
        }
        
        template <typename T>
        inline bool BatchChannelResult<T>::await_suspend(std::coroutine_handle<> handle)
        {
            auto wait_co = std::coroutine_handle<PromiseTypeBase>::from_address(handle.address()).promise().getCoroutine();
            if(auto co_ptr = wait_co.lock()) {
                co_ptr->modToSuspend();
            }
            m_channel.m_waker = Waker(wait_co);
            return true;
        }   

        template <typename T>
        inline std::optional<std::vector<T>> BatchChannelResult<T>::await_resume() const
        {
            auto co = m_channel.m_waker.getCoroutine();
            if(auto co_ptr = co.lock()) {
                co_ptr->modToRunning();
            }
            if(m_channel.m_queue.empty()) {
                return std::nullopt;
            }
            std::vector<T> values;
            size_t queue_size = m_channel.m_queue.size();
            values.reserve(queue_size);
            // 修复：应该使用 queue_size 而不是 values.size()（此时 values 还是空的）
            for(size_t i = 0; i < queue_size; ++i) {
                values.emplace_back(std::move(m_channel.m_queue.front()));
                m_channel.m_queue.pop();
            }
            values.shrink_to_fit();
            return values;
        }
    }

    template <typename T>
    inline bool AsyncChannel<T>::send(T&& value)
    {
        m_queue.push(std::move(value));
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
    inline details::ChannelResult<T> AsyncChannel<T>::recv()
    {
        return details::ChannelResult<T>(*this);
    }
    
    
    template <typename T>
    inline details::BatchChannelResult<T> AsyncChannel<T>::recvBatch()
    {
        return details::BatchChannelResult<T>(*this);
    }
}

#endif // GALAY_KERNEL_CONCURRENCY_SPSCCHANNEL_INL