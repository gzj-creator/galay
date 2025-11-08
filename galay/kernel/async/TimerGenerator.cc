#include "TimerGenerator.h"
#include <cassert>

namespace galay 
{
    TimerGenerator::ptr TimerGenerator::createPtr(CoSchedulerHandle handle)
    {
        return std::make_shared<TimerGenerator>(handle);
    }

    TimerGenerator::TimerGenerator(CoSchedulerHandle handle)
        : m_handle(handle)
    {
    }

    AsyncResult<nil> TimerGenerator::wait(std::chrono::milliseconds ms, const std::function<void()> &func)
    {
        if(ms < std::chrono::milliseconds::zero()) {
            return {nil()};
        }
        auto manager = m_handle.timerManager();
        return {std::make_shared<details::TimeWaitEvent>(manager, ms, func)};
    }

    AsyncResult<nil> TimerGenerator::sleep(std::chrono::milliseconds ms)
    {
        if(ms < std::chrono::milliseconds::zero()) {
            return {nil()};
        }
        auto manager = m_handle.timerManager();
        return {std::make_shared<details::SleepforEvent>(manager, ms)};
    }
}