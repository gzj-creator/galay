#include "Timer.h"
#include "galay/utils/System.h"

namespace galay 
{ 
    Timer::Timer(std::chrono::milliseconds ms, const std::function<void()>& callback)
        : m_callback(callback), m_deadline(std::chrono::time_point_cast<std::chrono::milliseconds>(\
            std::chrono::system_clock::now()).time_since_epoch().count() + ms.count()),\
            m_cancel(false)
    {
    }

    int64_t Timer::getDeadline() const
    {
        return m_deadline;
    }

    int64_t Timer::getRemainTime() const
    {
        int64_t now = utils::getCurrentTimeMs();
        const int64_t time = m_deadline - now;
        return time < 0 ? 0 : time;
    }

    void Timer::execute()
    {
        bool old = false;
        if(!m_cancel.compare_exchange_strong(old, true)) {
            return;
        }
        m_callback();
    }

    bool Timer::cancel()
    {
        bool old = false;
        return m_cancel.compare_exchange_strong(old, true);
    }
    
}