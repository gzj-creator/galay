//
// Created by gong on 2025/6/28.
//

#include "Runtime.h"
#include "galay/common/Common.h"
#include "galay/common/Log.h"
#include <thread>

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

    std::atomic_int32_t &RuntimeVisitor::index()
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
            setThreadName("CoroutineManager");
            run();
            LogTrace("CoroutineManager exit successfully!");
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
    }

    Runtime::Runtime(
        int eTimeout,
        int coSchedulerNum,
        EventScheduler::ptr eScheduler,
        TimerManager::ptr timerManager,
        std::chrono::milliseconds coManagerInterval
    )
        : m_eTimeout(eTimeout)
        , m_eScheduler(std::move(eScheduler))
        , m_timerManager(std::move(timerManager))
    {
        // 初始化协程调度器
        m_cSchedulers.reserve(coSchedulerNum);
        for(int i = 0; i < coSchedulerNum; ++i) {
            m_cSchedulers.emplace_back(CoroutineScheduler(std::make_unique<CoroutineConsumer>()));
        }
        
        // 初始化协程管理器（如果需要）
        if(coManagerInterval >= std::chrono::milliseconds::zero()) {
            m_cManager = std::make_unique<CoroutineManager>(coManagerInterval);
        }
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

    CoSchedulerHandle Runtime::getCoSchedulerHandle()
    {
        // 使用轮询选择一个调度器
        int old = -1;
        while (true)
        {
            old = m_index.load();
            if (m_index.compare_exchange_strong(old, (old + 1) % m_cSchedulers.size()))
            {
                break;
            }
        }
        return CoSchedulerHandle(&m_cSchedulers[old], this);
    }

    std::optional<CoSchedulerHandle> Runtime::getCoSchedulerHandle(Token token)
    {
        if(token >= static_cast<int>(m_cSchedulers.size())) {
            return std::nullopt;
        }
        return CoSchedulerHandle(&m_cSchedulers[token], this);
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

    bool Runtime::isRunning()
    {
        return m_running.load();
    }

    size_t Runtime::coSchedulerSize()
    {
        return m_cSchedulers.size();
    }

    CoSchedulerHandle Runtime::schedule(CoroutineBase::wptr co)
    {
        if(!m_eScheduler || m_cSchedulers.size() == 0) {
            throw std::runtime_error("Runtime not started");
        }
        int old = -1;
        while (true)
        {
            old = m_index.load();
            if (m_index.compare_exchange_strong(old, (old + 1) % m_cSchedulers.size()))
            {
                if(m_cManager) {
                    m_cManager->manage(co);
                }
                m_cSchedulers[old].resumeCoroutine(co);
                break;
            }
        }
        return CoSchedulerHandle(&m_cSchedulers[old], this);
    }

    CoSchedulerHandle Runtime::schedule(CoroutineBase::wptr co, Token token)
    {
        if(!m_eScheduler || m_cSchedulers.size() == 0) {
            throw std::runtime_error("Runtime not started");
        }
        if(token < 0 || token >= static_cast<int>(m_cSchedulers.size())) {
            throw std::runtime_error("Invalid token");
        }
        if(m_cManager) {
            m_cManager->manage(co);
        }
        m_cSchedulers[token].resumeCoroutine(co);
        return CoSchedulerHandle(&m_cSchedulers[token], this);
    }

    RuntimeBuilder &RuntimeBuilder::startCoManager(std::chrono::milliseconds interval)
    {
        m_interval = interval;
        return *this;
    }

    RuntimeBuilder &RuntimeBuilder::setEventCheckTimeout(int timeout)
    {
        m_eTimeout = timeout;
        return *this;
    }

    RuntimeBuilder &RuntimeBuilder::setCoSchedulerNum(int num)
    {
        m_coSchedulerNum = num;
        return *this;
    }

    RuntimeBuilder &RuntimeBuilder::useExternalEventScheduler(EventScheduler::ptr scheduler)
    {
        m_eventScheduler = scheduler;
        return *this;
    }

    RuntimeBuilder &RuntimeBuilder::setEventSchedulerInitFdsSize(int fds_set_size)
    {
        m_eventSchedulerInitFdsSize = fds_set_size;
        return *this;
    }

    Runtime RuntimeBuilder::build()
    {
        // 创建或使用现有的 EventScheduler
        EventScheduler::ptr eScheduler = m_eventScheduler 
            ? m_eventScheduler 
            : std::make_shared<EventScheduler>();
        
        // 创建 TimerManager 及其激活器
    #if defined(USE_EPOLL)
        auto activator = std::make_shared<EpollTimerActive>(eScheduler.get());
    #elif defined(USE_KQUEUE)
        auto activator = std::make_shared<KQueueTimerActive>(eScheduler.get());
    #elif defined(USE_IOURING)
        auto activator = std::make_shared<IOUringTimerActive>(eScheduler.get());
    #endif
        TimerManager::ptr timerManager = std::make_shared<PriorityQueueTimerManager>(activator);
        
        // 使用构造函数直接创建 Runtime
        return Runtime(
            m_eTimeout,
            m_coSchedulerNum,
            std::move(eScheduler),
            std::move(timerManager),
            m_interval
        );
    }

}