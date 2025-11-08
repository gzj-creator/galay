#include "CoSchedulerHandle.hpp"
#include "galay/kernel/runtime/Runtime.h"
#include "galay/kernel/async/AsyncFactory.h"

namespace galay
{
    EventScheduler* CoSchedulerHandle::eventScheduler() const
    {
        if (!m_runtime) return nullptr;
        RuntimeVisitor visitor(*m_runtime);
        return visitor.eventScheduler().get();
    }

    TimerManager* CoSchedulerHandle::timerManager() const
    {
        if (!m_runtime) return nullptr;
        RuntimeVisitor visitor(*m_runtime);
        return visitor.timerManager().get();
    }

    AsyncFactory CoSchedulerHandle::getAsyncFactory() const
    {
        return AsyncFactory(*this);
    }
}

