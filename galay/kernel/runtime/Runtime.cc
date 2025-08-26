//
// Created by gong on 2025/6/28.
//

#include "Runtime.h"

namespace galay
{

    RuntimeVisitor::RuntimeVisitor(Runtime &runtime)
        :m_runtime(runtime)
    {
    }

    EventScheduler::ptr RuntimeVisitor::eventScheduler()
    {
        return m_runtime.m_eScheduler;
    }

    TimerManager::ptr RuntimeVisitor::timerManager()
    {
        return m_runtime.m_timerManager;
    }

    int &RuntimeVisitor::eventCheckTimeout()
    {
        return m_runtime.m_eTimeout;
    }

    std::atomic_size_t &RuntimeVisitor::index()
    {
        return m_runtime.m_index;
    }

    CoroutineManager::uptr &RuntimeVisitor::coManager()
    {
        return m_runtime.m_cManager;
    }

    std::vector<CoroutineScheduler> &RuntimeVisitor::coScheduler()
    {
        return m_runtime.m_cSchedulers;
    }

    CoroutineManager::CoroutineManager(std::chrono::milliseconds interval)
        :m_interval(interval)
    {
    }

    CoroutineManager::CoroutineManager(CoroutineManager &&cm)
    {
        if(this != &cm) {
            m_interval = std::move(cm.m_interval);
            m_thread = std::move(cm.m_thread);
            m_change.store(cm.m_change.load());
            m_queue_1 = std::move(cm.m_queue_1);
            m_queue_2 = std::move(cm.m_queue_2);
            m_running.store(cm.m_running.load());
        }
    }

    CoroutineManager &CoroutineManager::operator=(CoroutineManager &&cm)
    {
        if(this != &cm) { 
            m_interval = std::move(cm.m_interval);
            m_thread = std::move(cm.m_thread);
            m_change.store(cm.m_change.load());
            m_queue_1 = std::move(cm.m_queue_1);
            m_queue_2 = std::move(cm.m_queue_2);
            m_running.store(cm.m_running.load());
        }
        return *this;
    }

    void CoroutineManager::start()
    {
        m_running = true;
        m_thread = std::thread([this]() {
            pthread_setname_np(pthread_self(), "CoroutineManager");
            run();
            LogTrace("CoroutineManager exit successfully!");
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
                co.lock()->destroy();
            }
        }
        while (m_queue_2.try_dequeue(co))
        {
            if(!co.expired()) {
                co.lock()->destroy();
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

    Runtime::Runtime()
    {
        m_eScheduler = std::make_shared<EventScheduler>(DEFAULT_FDS_SET_INITIAL_SIZE);
        for(size_t i = 0; i < DEFAULT_COS_SCHEDULER_THREAD_NUM; ++i) {
            CoroutineConsumer::uptr consumer = CoroutineConsumer::create();
            m_cSchedulers.emplace_back(CoroutineScheduler(std::move(consumer)));
        }
    #if defined(USE_EPOLL)
        auto activator = std::make_shared<EpollTimerActive>(m_eScheduler.get());
    #endif
        m_timerManager = std::make_shared<PriorityQueueTimerManager>(activator);
    }

    Runtime::Runtime(Runtime &&rt)
    {
        if(this != &rt) {
            m_eScheduler = std::move(rt.m_eScheduler);
            m_timerManager = std::move(rt.m_timerManager);
            m_cSchedulers = std::move(rt.m_cSchedulers);
            m_cManager = std::move(rt.m_cManager);
            m_eTimeout = std::move(rt.m_eTimeout);
            m_index.store(rt.m_index.load());
            rt.m_index.store(0);
            m_running.store(rt.m_running.load());
            rt.m_running.store(false);
        }
    }

    Runtime &Runtime::operator=(Runtime &&rt)
    {
        if(this != &rt) {
            m_eScheduler = std::move(rt.m_eScheduler);
            m_timerManager = std::move(rt.m_timerManager);
            m_cSchedulers = std::move(rt.m_cSchedulers);
            m_cManager = std::move(rt.m_cManager);
            m_eTimeout = std::move(rt.m_eTimeout);
            m_index.store(rt.m_index.load());
            rt.m_index.store(0);
            m_running.store(rt.m_running.load());
            rt.m_running.store(false);
        }
        return *this;
    }

    void Runtime::startCoManager(std::chrono::milliseconds interval)
    {
        if(interval >= std::chrono::milliseconds::zero()) {
            size_t num = m_cSchedulers.size();
            if(num == 0) {
                throw std::runtime_error("no coroutine scheduler");
            }
            m_cManager = std::make_unique<CoroutineManager>(interval);
        } else {
            m_cManager.reset();
        }
    }

    // 默认不启动coroutineManager
    void Runtime::start()
    {   
        if(m_cSchedulers.size() == 0) {
            for(int i = 0; i < DEFAULT_COS_SCHEDULER_THREAD_NUM; ++i) {
                CoroutineConsumer::uptr consumer = CoroutineConsumer::create();
                m_cSchedulers.emplace_back(CoroutineScheduler(std::move(consumer)));
            }
        }
        m_eScheduler->start(m_eTimeout);
        m_timerManager->start();
        for(auto& scheduler: m_cSchedulers) scheduler.start();
        if(m_cManager) m_cManager->start();
        m_running.store(true);
    }

    void Runtime::stop()
    {
        if(m_running.load()) {
            m_running.store(false);
        } else {
            return;
        }
        if(!m_eScheduler || m_cSchedulers.empty()) {
            LogError("Runtime not started");
            throw std::runtime_error("Runtime not started");
        }
        if(m_cManager) m_cManager->stop();
        for(auto& scheduler: m_cSchedulers) {
            scheduler.stop();
        }
        m_cSchedulers.clear();
        if(m_timerManager) {
            m_timerManager->stop();
            m_timerManager.reset();
        }
        if(m_eScheduler) {
            m_eScheduler->stop();
            m_eScheduler.reset();
        }
    }

    size_t Runtime::coSchedulerSize()
    {
        return m_cSchedulers.size();
    }

    RuntimeBuilder &RuntimeBuilder::startCoManager(std::chrono::milliseconds interval)
    {
        m_runtime.startCoManager(interval);
        return *this;
    }

    RuntimeBuilder &RuntimeBuilder::setEventCheckTimeout(int timeout)
    {
        m_runtime.m_eTimeout = timeout;
        return *this;
    }

    RuntimeBuilder &RuntimeBuilder::setCoSchedulerNum(int num)
    {
        m_runtime.m_cSchedulers.clear();
        for(int i = 0; i < num; ++i) {
            m_runtime.m_cSchedulers.emplace_back(CoroutineScheduler(std::make_unique<CoroutineConsumer>()));
        }
        return *this;
    }

    RuntimeBuilder &RuntimeBuilder::useExternalEventScheduler(EventScheduler::ptr scheduler)
    {
        m_runtime.m_eScheduler = scheduler;
        return *this;
    }

    RuntimeBuilder &RuntimeBuilder::setEventSchedulerInitFdsSize(int fds_set_size)
    {
        m_runtime.m_eScheduler = std::make_shared<EventScheduler>(fds_set_size);
        return *this;
    }

    Runtime RuntimeBuilder::build()
    {
        Runtime rt = std::move(m_runtime);
        m_runtime = Runtime();
        return rt;
    }

}