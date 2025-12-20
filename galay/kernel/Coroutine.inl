#ifndef GALAY_COROUTINE_INL
#define GALAY_COROUTINE_INL

#include "Coroutine.hpp"


namespace galay
{

    template<CoType T>
    inline bool WaitResult<T>::await_suspend(std::coroutine_handle<> handle)
    {
        m_wait_co = std::coroutine_handle<PromiseTypeBase>::from_address(handle.address()).promise().getCoroutine();
        if(auto co_ptr = m_wait_co.lock()) {
            co_ptr->modToSuspend();
            m_coroutine.then(m_wait_co);
            co_ptr->belongEngine()->spwan(m_coroutine.origin());
            return true;
        }
        return false;
    }


    template <CoType T>
    inline Coroutine<T> PromiseType<T>::get_return_object() noexcept
    {
        m_coroutine = std::make_shared<Coroutine<T>>(std::coroutine_handle<PromiseType>::from_promise(*this));
        return *m_coroutine;
    }

    template<CoType T>
    inline std::suspend_always PromiseType<T>::yield_value(YieldValue&& value) noexcept
    {
        m_coroutine->modToSuspend();
        if(value.re_scheduler) {
            m_coroutine->belongEngine()->spwan(m_coroutine);
        }
        return {};
    }

    template<CoType T>
    inline void PromiseType<T>::return_value(T&& value) const noexcept
    {
        m_coroutine->m_data->m_result = std::move(value);
        m_coroutine->modToFinished();
    }


    template<CoType T>
    inline PromiseType<T>::~PromiseType()
    {
        if(m_coroutine) {
            m_coroutine->executeDeferTask();
        }
    }

    template<CoType T>
    inline Coroutine<T>::Coroutine(const std::coroutine_handle<promise_type> handle) noexcept
    {
        m_handle = handle;
        m_data = std::make_shared<CoroutineData>();
    }

    template<CoType T>
    inline Coroutine<T>::Coroutine(Coroutine&& other) noexcept
    {
        m_handle = other.m_handle;
        other.m_handle = nullptr;
        m_data = other.m_data;
        other.m_data.reset();
    }

    template<CoType T>
    inline Coroutine<T>::Coroutine(const Coroutine& other) noexcept
    {
        m_handle = other.m_handle;
        m_data = other.m_data;
    }

    template<CoType T>
    inline bool Coroutine<T>::isRunning() const
    {
        return m_data->m_status.load(std::memory_order_relaxed) == CoroutineStatus::Running;
    }

    template<CoType T>
    inline bool Coroutine<T>::isSuspend() const
    {
        return m_data->m_status.load(std::memory_order_relaxed) == CoroutineStatus::Suspended;
    }

    template<CoType T>
    inline bool Coroutine<T>::isWaking() const
    {
        return m_data->m_status.load(std::memory_order_relaxed) == CoroutineStatus::Waking;
    }

    template<CoType T>
    inline bool Coroutine<T>::isDestroying() const
    {
        return m_data->m_status.load(std::memory_order_relaxed) == CoroutineStatus::Destroying;
    }

    template<CoType T>
    inline bool Coroutine<T>::isDone() const
    {
        return m_data->m_status.load(std::memory_order_relaxed) == CoroutineStatus::Finished;
    }

    template <CoType T>
    inline Engine *Coroutine<T>::belongEngine() const
    {
        return m_data->m_scheduler.load(std::memory_order_relaxed);
    }

    template <CoType T>
    inline CoroutineBase& Coroutine<T>::then(CoroutineBase::wptr co)
    {
        m_data->m_next = co;
        return *this;
    }

    template <CoType T>
    inline std::optional<T> Coroutine<T>::result()
    {
        return m_data->m_result;
    }

    template <CoType T>
    inline void Coroutine<T>::belongEngine(Engine* scheduler)
    {
        m_data->m_scheduler.store(scheduler, std::memory_order_relaxed);
    }


    template<CoType T>
    inline std::optional<T> Coroutine<T>::operator()()
    {
        return m_data->m_result;
    }

    template<CoType T>
    inline CoroutineBase::wptr Coroutine<T>::origin()
    {
        return m_handle.promise().getCoroutine();
    }

    template<CoType T>
    inline WaitResult<T> Coroutine<T>::wait()
    {
        return WaitResult<T>(*this);
    }


    template<CoType T>
    inline void Coroutine<T>::destroy()
    {
        m_handle.destroy();
    }

    template<CoType T>
    inline void Coroutine<T>::resume()
    {
        return m_handle.resume();
    }


    template <CoType T>
    inline void Coroutine<T>::executeDeferTask()
    {
        if(auto next = m_data->m_next.lock()) {
            next->belongEngine()->spwan(next);
            m_data->m_next.reset();
        }
    }


}


#endif