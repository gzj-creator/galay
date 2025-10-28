#ifndef GALAY_SCHEDULER_HOLDER_INL
#define GALAY_SCHEDULER_HOLDER_INL

namespace galay::details
{

template<LoadBalancerType Type>
inline void SchedulerHolder<Type>::initialize(std::vector<std::unique_ptr<typename Type::value_type>>&& schedulers)
{
    m_schedulers = std::forward<decltype(schedulers)>(schedulers);
}

template<LoadBalancerType Type>
inline void SchedulerHolder<Type>::startAll()
{
    std::vector<typename Type::value_type*> scheduler_ptrs;
    int i = 0;
    scheduler_ptrs.reserve(m_schedulers.size());
    for( auto& scheduler : m_schedulers) {
        int try_count = MAX_START_SCHEDULERS_RETRY;
        scheduler_ptrs.push_back(scheduler.get());
        scheduler->start();
        while(!scheduler->isRunning() && try_count-- >= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if(try_count <= 0) {
            LogTrace("start coroutine scheduler failed, index: {}", i);
            exit(-1);
        }
        ++i;
    }
    SchedulerLoadBalancer = std::make_unique<Type>(scheduler_ptrs);
}

template<LoadBalancerType Type>
inline void SchedulerHolder<Type>::stopAll()
{
    for( auto& scheduler : m_schedulers) {
        scheduler->stop();
    }
}

template<LoadBalancerType Type>
inline SchedulerHolder<Type>* SchedulerHolder<Type>::getInstance()
{
    if(!Instance) {
        Instance = std::make_unique<SchedulerHolder>();
    }
    return Instance.get();
}

template<LoadBalancerType Type>
inline typename Type::value_type* SchedulerHolder<Type>::getScheduler()
{
    return SchedulerLoadBalancer.get()->select();
}

template<LoadBalancerType Type>
inline typename Type::value_type* SchedulerHolder<Type>::getScheduler(uint32_t index)
{
    return m_schedulers[index].get();
}

template <LoadBalancerType Type>
inline int SchedulerHolder<Type>::getSchedulerSize()
{
    return m_schedulers.size();
}
}



#endif