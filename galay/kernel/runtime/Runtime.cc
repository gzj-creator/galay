//
// Created by gong on 2025/6/28.
//

#include "Runtime.h"

namespace galay
{
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

    Runtime::Runtime(bool start_check, std::chrono::milliseconds check_interval, TimerManagerType type)
        :m_scheduler(CoroutineSchedulerFactory::create(CoroutineConsumer::create(), type))
    {
        m_scheduler->start();
        if(start_check) {
            m_manager = std::make_unique<CoroutineManager>(m_scheduler.get(), check_interval);
            m_manager->start();
        }
    }

    Runtime::~Runtime()
    {
        if(m_manager) m_manager->stop();
        m_scheduler->stop();
    }

}