#ifndef GALAY_LIMIT_WAITER_HPP
#define GALAY_LIMIT_WAITER_HPP

#include "Result.hpp"
#include "galay/common/Common.h"
#include "CoSchedulerHandle.hpp"
#include <list>
#include <atomic>

namespace galay
{
    template<typename T, typename E>
    class LimitWaiter;

    namespace details
    {
        template<typename T, typename E>
        class LimitWaitEvent: public AsyncEvent<std::expected<T, E>>
        {
            template<typename M, typename F>
            friend class galay::LimitWaiter;
        public:
            LimitWaitEvent(LimitWaiter<T, E>& waiter);
            //return true while not suspend
            bool onReady() override;
            //return true while suspend
            bool onSuspend(Waker waker) override;

            template<CoType Type>
            void appendTask(Coroutine<Type>&& co);
        private:
            LimitWaiter<T, E>& m_waiter;
            std::shared_ptr<std::list<CoroutineBase::wptr>> m_tasks;
        };
    }

    /**
     * @brief 限制型等待器，只允许一个任务被notify成功，其他任务被销毁
     * @details 用于timeout等场景，确保多个竞争的任务中只有一个能完成，
     *          其余任务由CoScheduler统一销毁，避免内存泄漏
     */
    template<typename T, typename E>
    class LimitWaiter
    {
        template<typename M, typename F>
        friend class details::LimitWaitEvent;
    public:
        LimitWaiter();
        template<CoType Type>
        void appendTask(Coroutine<Type>&& co);
        AsyncResult<std::expected<T, E>> wait();
        /**
         * @brief 尝试通知，只有第一个调用成功
         * @return 返回true表示此次notify成功（获得通知权），应该销毁其他任务
         *         返回false表示已有其他任务先成功notify，此任务应该什么都不做
         */
        bool notify(std::expected<T, E>&& value);

        /**
         * @brief 销毁除了当前任务外的其他所有任务
         */
        void destroyTasks();

    private:
        Waker m_waker;
        std::atomic_bool m_notified = false;
        std::shared_ptr<details::LimitWaitEvent<T, E>> m_event;
    };

    template <typename T, typename E>
    inline LimitWaiter<T, E>::LimitWaiter()
        : m_event(std::make_shared<details::LimitWaitEvent<T, E>>(*this))
    {
    }

    template <typename T, typename E>
    inline AsyncResult<std::expected<T, E>> LimitWaiter<T, E>::wait()
    {
        return {this->m_event};
    }

    template <typename T, typename E>
    inline bool LimitWaiter<T, E>::notify(std::expected<T, E> &&value)
    {
        bool expected = false;
        if(m_notified.compare_exchange_strong(expected, true,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
            m_event->m_result = std::move(value);
            m_waker.wakeUp();
            destroyTasks();
            return true;
        }
        return false;
    }

    template <typename T, typename E>
    template <CoType Type>
    inline void LimitWaiter<T, E>::appendTask(Coroutine<Type> &&co)
    {
        m_event->appendTask(std::move(co));
    }

    template <typename T, typename E>
    inline void LimitWaiter<T, E>::destroyTasks()
    {
        if(m_event->m_tasks) {
            for(auto it = m_event->m_tasks->begin(); it != m_event->m_tasks->end(); ++it) {
                it->lock()->belongScheduler()->destroyCoroutine(*it);
            }
        }
    }
}

namespace galay::details
{
    template <typename T, typename E>
    inline LimitWaitEvent<T, E>::LimitWaitEvent(LimitWaiter<T, E> &waiter)
        : m_waiter(waiter)
    {
    }

    template <typename T, typename E>
    inline bool LimitWaitEvent<T, E>::onReady()
    {
        return false;
    }

    template <typename T, typename E>
    inline bool LimitWaitEvent<T, E>::onSuspend(Waker waker)
    {
        m_waiter.m_waker = waker;
        if(m_tasks) {
            for(auto it = m_tasks->begin(); it != m_tasks->end(); ++it) {
                waker.belongScheduler()->schedule(*it);
            }
        }
        return true;
    }

    template <typename T, typename E>
    template <CoType Type>
    inline void LimitWaitEvent<T, E>::appendTask(Coroutine<Type> &&co)
    {
        if(!m_tasks) {
            m_tasks = std::make_shared<std::list<CoroutineBase::wptr>>();
        }
        m_tasks->emplace_back(co.origin());
    }
}

#endif