#include "TimerGenerator.h"
#if defined(USE_EPOLL)
    #include <sys/timerfd.h>
#endif


namespace galay 
{
    TimerGenerator::TimerGenerator(EventScheduler *scheduler)

    {
    #if defined(USE_EPOLL)
        m_context.m_active = std::make_shared<EpollTimerActive>(scheduler);
        m_context.m_manager = std::make_shared<PriorityQueueTimerManager>();
        m_context.m_handle.fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    #endif
    }

    AsyncResult<ValueWrapper<bool>> TimerGenerator::close()
    {
        return {std::make_shared<details::CloseTimeEvent>(m_context)};
    }

    TimerGenerator::~TimerGenerator()
    {
    
    }
}