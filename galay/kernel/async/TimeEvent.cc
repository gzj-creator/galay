#include "TimeEvent.h"
#include "galay/utils/System.h"



namespace galay::details
{
    bool CloseTimeEvent::ready()
    {
        return false;
    }

    bool CloseTimeEvent::suspend(Waker waker)
    {
        using namespace error;
        Error::ptr error = nullptr;
        bool success = true;
        if(m_context.m_handle.flags[0] == 1)  m_context.m_scheduler->delEvent(this, nullptr);
        if(::close(m_context.m_handle.fd))
        {
            error = std::make_shared<SystemError>(error::ErrorCode::CallCloseError, errno);
            success = false;
        } else {
            m_context.m_handle = GHandle::invalid();
        }
        makeValue(m_result, std::move(success), error);
        return false;
    }
}