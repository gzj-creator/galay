#ifndef GALAY_ASYNC_QUEUE_HPP
#define GALAY_ASYNC_QUEUE_HPP

#include "Result.hpp"
#include "galay/common/Common.h"
#include "CoScheduler.hpp"
#include <queue>
#include <list>

namespace galay::mpsc
{
    template<typename T, typename E>
    class AsyncQueue;

    namespace details
    {
        template<typename T, typename E>
        class DequeueEvent: public AsyncEvent<std::expected<T, E>>
        {
            template<typename M, typename F>
            friend class galay::mpsc::AsyncQueue;
        public:
            DequeueEvent(AsyncQueue<T, E>& queue);
            //return true while not suspend
            bool onReady() override;
            //return true while suspend
            bool onSuspend(Waker waker) override;
            // 被唤醒时从队列取数据
            std::expected<T, E> onResume() override;
        private:
            AsyncQueue<T, E>& m_queue;
        };
    }

    template<typename T, typename E>
    class AsyncQueue
    {
        template<typename M, typename F>
        friend class details::DequeueEvent;
    public:
        AsyncQueue();

        // 异步出队操作，如果队列为空则挂起等待
        AsyncResult<std::expected<T, E>> waitDequeue();

        // 同步入队操作（移动版本），如果有等待者则立即唤醒
        void emplace(T&& value);

        // 同步入队操作（拷贝版本），如果有等待者则立即唤醒
        void push(const T& value);

        // 获取队列大小
        size_t size() const;

        // 检查队列是否为空
        bool empty() const;

        // 检查是否有协程在等待
        bool isWaiting() const;

    private:
        std::queue<T> m_queue;              // 数据队列
        Waker m_waker;                       // 消费者的waker
        std::atomic_bool m_waiting = false;  // 是否有消费者在等待
    };

    // AsyncQueue 实现
    template <typename T, typename E>
    inline AsyncQueue<T, E>::AsyncQueue()
    {
    }

    template <typename T, typename E>
    inline AsyncResult<std::expected<T, E>> AsyncQueue<T, E>::waitDequeue()
    {
        // 每次wait创建新的event，不再保存到成员变量
        // 因为多个协程可能同时wait，每个都需要独立的event
        auto new_event = std::make_shared<details::DequeueEvent<T, E>>(*this);
        return {new_event};
    }

    template <typename T, typename E>
    inline void AsyncQueue<T, E>::emplace(T &&value)
    {
        m_queue.push(std::move(value));

        // 如果有消费者在等待，唤醒它（消费者会从队列中取数据）
        bool expected = true;
        if(m_waiting.compare_exchange_strong(expected, false,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
            m_waker.wakeUp();
        }
    }

    template <typename T, typename E>
    inline void AsyncQueue<T, E>::push(const T &value)
    {
        m_queue.push(value);

        // 如果有消费者在等待，唤醒它（消费者会从队列中取数据）
        bool expected = true;
        if(m_waiting.compare_exchange_strong(expected, false,
                                            std::memory_order_acq_rel,
                                            std::memory_order_acquire)) {
            m_waker.wakeUp();
        }
    }

    template <typename T, typename E>
    inline size_t AsyncQueue<T, E>::size() const
    {
        return m_queue.size();
    }

    template <typename T, typename E>
    inline bool AsyncQueue<T, E>::empty() const
    {
        return m_queue.empty();
    }

    template <typename T, typename E>
    inline bool AsyncQueue<T, E>::isWaiting() const
    {
        return m_waiting.load();
    }
}

namespace galay::mpsc::details
{
    template <typename T, typename E>
    inline DequeueEvent<T, E>::DequeueEvent(AsyncQueue<T, E> &queue)
        : m_queue(queue)
    {
    }

    template <typename T, typename E>
    inline bool DequeueEvent<T, E>::onReady()
    {
        // 如果队列中有数据，直接返回不挂起
        if(!m_queue.m_queue.empty()) {
            this->m_result = std::expected<T, E>{std::move(m_queue.m_queue.front())};
            m_queue.m_queue.pop();
            return true;
        }
        return false;
    }

    template <typename T, typename E>
    inline bool DequeueEvent<T, E>::onSuspend(Waker waker)
    {
        // 再次检查队列是否有数据（避免竞态）
        if(!m_queue.m_queue.empty()) {
            this->m_result = std::expected<T, E>{std::move(m_queue.m_queue.front())};
            m_queue.m_queue.pop();
            return false; // 不挂起
        }

        // 队列为空，将waker设置为等待的消费者并挂起
        m_queue.m_waker = waker;
        bool expected = false;
        if(!m_queue.m_waiting.compare_exchange_strong(expected, true,
                                                     std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
            LogTrace("DequeueEvent::onSuspend: already waiting (multiple consumers not supported)");
            // 已经有其他消费者在等待，返回错误
            this->m_result = std::unexpected(E(0, 0));
            return false;
        }

        return true; // 挂起
    }

    template <typename T, typename E>
    inline std::expected<T, E> DequeueEvent<T, E>::onResume()
    {
        // 被唤醒后从队列中取数据
        if(!m_queue.m_queue.empty()) {
            auto result = std::expected<T, E>{std::move(m_queue.m_queue.front())};
            m_queue.m_queue.pop();
            return result;
        }
        // 队列为空（不应该发生），返回错误
        return std::unexpected(E(0, 0));
    }
}

#endif