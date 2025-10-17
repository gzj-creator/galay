#ifndef GALAY_COROUTINE_INL
#define GALAY_COROUTINE_INL

#include "Coroutine.hpp"


namespace galay
{


    template <CoType T>
    inline Coroutine<T> PromiseType<T>::get_return_object() noexcept
    {
        m_coroutine = std::make_shared<Coroutine<T>>(std::coroutine_handle<PromiseType>::from_promise(*this));
        return *m_coroutine;
    }

    template<CoType T>
    inline std::suspend_always PromiseType<T>::yield_value(T&& value) noexcept
    {
        *(m_coroutine->m_data->m_result) = std::move(value);
        m_coroutine->become(CoroutineStatus::Suspended);
        return {};
    }

    template<CoType T>
    inline void PromiseType<T>::return_value(T&& value) const noexcept
    {
        *(m_coroutine->m_data->m_result) = std::move(value);
    }


    template<CoType T>
    inline PromiseType<T>::~PromiseType()
    {
        CoroutineStatus origin = m_coroutine->m_data->m_status.load();
        if(!m_coroutine->m_data->m_status.compare_exchange_strong(origin, CoroutineStatus::Finished)) {
            return;
        }
        if(m_coroutine) {
            m_coroutine->exitToExecuteDeferStk();
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
    inline Coroutine<T>& Coroutine<T>::operator=(Coroutine&& other) noexcept
    {
        m_handle = other.m_handle;
        other.m_handle = nullptr;
        m_data = other.m_data;
        other.m_data.reset();
        return *this;
    }

    template<CoType T>
    Coroutine<T>& Coroutine<T>::operator=(const Coroutine& other) noexcept
    {
        m_handle = other.m_handle;
        m_data  = other.m_data;
        return *this;
    }


    template<CoType T>
    inline bool Coroutine<T>::isRunning() const
    {
        return m_data->m_status.load() == CoroutineStatus::Running;
    }

    template<CoType T>
    inline bool Coroutine<T>::isSuspend() const
    {
        return m_data->m_status.load() == CoroutineStatus::Suspended;
    }

    template<CoType T>
    inline bool Coroutine<T>::isDone() const
    {
        return m_data->m_status.load() == CoroutineStatus::Finished;
    }

    template <CoType T>
    inline CoroutineScheduler *Coroutine<T>::belongScheduler() const
    {
        return m_data->m_scheduler.load();
    }

    template <CoType T>
    inline CoroutineBase& Coroutine<T>::then(const Handler &callback)
    {
        m_data->m_cbs.push(callback);
        return *this;
    }

    template <CoType T>
    inline std::optional<T> Coroutine<T>::result()
    {
        return m_data->m_result;
    }

    template <CoType T>
    inline bool Coroutine<T>::become(CoroutineStatus status)
    {
        auto origin = m_data->m_status.load();
        if(!m_data->m_status.compare_exchange_strong(origin, status)) {
            return false;
        }
        return true;
    }

    template <CoType T>
    inline void Coroutine<T>::belongScheduler(CoroutineScheduler* scheduler)
    {
        m_data->m_scheduler.store(scheduler);
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
    inline void Coroutine<T>::exitToExecuteDeferStk()
    {
        while(!m_data->m_cbs.empty()) {
            m_data->m_cbs.front()();
            m_data->m_cbs.pop();
        }
    }


    template <CoType T>
    inline CoroutineDataVisitor<T>::CoroutineDataVisitor(CoroutineBase::wptr coroutine)
        : m_coroutine(coroutine)
    {
    }

    template <CoType T>
    inline bool CoroutineDataVisitor<T>::setResult(T&& result) {
        m_coroutine.lock()->template implCast<T>()->m_data->m_result = std::move(result);
        return true;
    }

    template <CoType T>
    inline bool CoroutineDataVisitor<T>::setStatus(CoroutineStatus status) {
        m_coroutine.lock()->template implCast<T>()->m_data->m_status.store(status);
        return true;
    }

    template <CoType T>
    inline bool CoroutineDataVisitor<T>::setScheduler(CoroutineScheduler *scheduler)
    {
        m_coroutine.lock()->template implCast<T>()->m_data->m_scheduler = scheduler;
        return true;
    }

}


#endif