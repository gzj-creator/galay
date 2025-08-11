//
// Created by gong on 2025/6/28.
//

#include "Runtime.h"

namespace galay
{
    RuntimeConfig::RuntimeConfig(Runtime &runtime)
        :m_runtime(runtime)
    {
    }

    RuntimeConfig &RuntimeConfig::eventTimeout(int64_t timeout)
    {
        m_runtime.m_event_timeout = timeout;
        return *this;
    }

    RuntimeConfig &RuntimeConfig::startCoManager(bool start)
    {
        m_runtime.m_start_check_co = start;
        return *this;
    }

    CoroutineManager::CoroutineManager(CoroutineScheduler *scheduler, std::chrono::milliseconds interval)
        :m_scheduler(scheduler), m_interval(interval)
    {
    }

    void CoroutineManager::start()
    {
        m_running = true;
        m_thread = std::thread([this]() {
            run();
        });
    }

    void CoroutineManager::manage(CoroutineBase::wptr co)
    {
        if(!m_change.load()) {
            m_queue_1.enqueue(co);
        } else {
            m_queue_2.enqueue(co);
        }
    }

    void CoroutineManager::stop()
    {
        m_running.store(false);
        m_thread.join();
        CoroutineBase::wptr co;
        while (m_queue_1.try_dequeue(co))
        {
            if(!co.expired()) {
                m_scheduler->destroyCoroutine(co);
            }
        }
        while (m_queue_2.try_dequeue(co))
        {
            if(!co.expired()) {
                m_scheduler->destroyCoroutine(co);
            }
        }
    }

    void CoroutineManager::run()
    {
        while (m_running.load())
        {
            autoCheck();
            std::this_thread::sleep_for(m_interval);
        }
    }

    void CoroutineManager::autoCheck()
    {
        bool old = m_change.load();
        while (m_change.compare_exchange_strong(old, !old)) {}
        if(!old) {
            CoroutineBase::wptr co;
            while (m_queue_1.try_dequeue(co))
            {
                if(!co.expired()) {
                    m_queue_2.enqueue(co);
                } 
            }
        } else {
            CoroutineBase::wptr co;
            while (m_queue_2.try_dequeue(co))
            {
                if(!co.expired()) {
                    m_queue_1.enqueue(co);
                } 
            }

        }
        
    }

    RuntimeConfig Runtime::config()
    {
        return RuntimeConfig(*this);
    }

    void Runtime::start()
    {
        m_eScheduler = std::make_shared<EventScheduler>();
        m_eScheduler->start(m_event_timeout);
        m_cScheduler = std::make_unique<CoroutineScheduler>(CoroutineConsumer::create());
        m_cScheduler->start();
        if(m_start_check_co) {
            m_manager = std::make_unique<CoroutineManager>(m_cScheduler.get(), m_co_check_interval); 
            m_manager->start();
        }
    }

    void Runtime::stop()
    {
        if(!m_eScheduler || !m_cScheduler) {
            throw std::runtime_error("Runtime not started");
        }
        if(m_manager) {
            m_manager->stop();
            m_manager.reset();
        }
        m_cScheduler->stop();
        m_cScheduler.reset();
        m_eScheduler->stop();
        m_eScheduler.reset();
    }

    Runtime::~Runtime()
    {
        if(m_eScheduler || m_cScheduler) {
            stop();
        }
    }
}