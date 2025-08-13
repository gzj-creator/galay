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

    RuntimeConfig &RuntimeConfig::startCoManager(bool start, std::chrono::milliseconds interval)
    {
        if(start) m_runtime.m_manager = std::make_unique<CoroutineManager>(m_runtime.m_cScheduler.get(), interval); 
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

    Runtime::Runtime(int fds_initial_size)
    {
        m_eScheduler = std::make_shared<EventScheduler>(fds_initial_size);
        m_cScheduler = std::make_unique<CoroutineScheduler>(CoroutineConsumer::create());
    #if defined(USE_EPOLL)
        auto activator = std::make_shared<EpollTimerActive>(m_eScheduler.get());
    #endif
        m_timerManager = std::make_shared<PriorityQueueTimerManager>(activator);
   
    }

    void Runtime::start()
    {   
        m_eScheduler->start(m_event_timeout);
        m_cScheduler->start();
        m_timerManager->start();
        if(m_manager) m_manager->start();
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
        m_timerManager->stop();
        m_timerManager.reset();
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