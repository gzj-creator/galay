//
// Created by gong on 2025/6/28.
//

#include "Runtime.h"

namespace galay
{
    Runtime::Runtime()
        :m_scheduler(CoroutineSchedulerFactory::create(CoroutineConsumer::create()))
    {
        m_scheduler->start();
    }

    void Runtime::modifyTimerManagerType(TimerManagerType type)
    {
        m_scheduler->getEventScheduler()->makeTimeEvent(type);
    }

    Runtime::~Runtime()
    {
        m_scheduler->stop();
    }
}