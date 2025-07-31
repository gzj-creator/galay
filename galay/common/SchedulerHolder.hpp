#ifndef GALAY_SCHEDULER_HOLDER_HPP
#define GALAY_SCHEDULER_HOLDER_HPP

#include "Base.h"
#include "Error.h"
#include "Log.h"

namespace galay::details
{



template<typename T>
concept LoadBalancerType = requires(T t)
{
    typename T::value_type;
    requires std::is_constructible_v<T, const std::vector<typename T::value_type*>&>;
    requires std::is_same_v<decltype(t.select()), typename T::value_type*>;
};

#define MAX_START_SCHEDULERS_RETRY   50

template<LoadBalancerType Type>
class SchedulerHolder
{
    static std::unique_ptr<Type> SchedulerLoadBalancer;
    static std::unique_ptr<SchedulerHolder> Instance;
public:

    void initialize(std::vector<std::unique_ptr<typename Type::value_type>>&& schedulers);
    void startAll();
    void stopAll();
    static SchedulerHolder* getInstance();
    Type::value_type* getScheduler();
    Type::value_type* getScheduler(uint32_t index);
    int getSchedulerSize();
private:
    std::vector<std::unique_ptr<typename Type::value_type>> m_schedulers;
};

template<LoadBalancerType Type>
std::unique_ptr<Type> SchedulerHolder<Type>::SchedulerLoadBalancer;

template<LoadBalancerType Type>
std::unique_ptr<SchedulerHolder<Type>> SchedulerHolder<Type>::Instance;


}

#include "SchedulerHolder.inl"

#endif