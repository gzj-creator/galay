#include "TimerGenerator.h"


namespace galay 
{
    TimerGenerator::ptr TimerGenerator::createPtr(Runtime &runtime)
    {
        return std::make_shared<TimerGenerator>(runtime);
    }

    TimerGenerator::TimerGenerator(Runtime& runtime)
        :m_runtime(runtime)
    {
    }

    AsyncResult<nil> TimerGenerator::wait(std::chrono::milliseconds ms, const std::function<void()> &func)
    {
        if(ms < std::chrono::milliseconds::zero()) {
            return {nil()};
        }
        RuntimeVisitor visitor(m_runtime);
        auto manager = visitor.timerManager().get();
        return {std::make_shared<details::TimeWaitEvent>(manager, ms, func)};
    }

    AsyncResult<nil> TimerGenerator::sleep(std::chrono::milliseconds ms)
    {
        if(ms < std::chrono::milliseconds::zero()) {
            return {nil()};
        }
        RuntimeVisitor visitor(m_runtime);
        auto manager = visitor.timerManager().get();
        return {std::make_shared<details::SleepforEvent>(manager, ms)};
    }

    TimerGenerator::TimerGenerator(const TimerGenerator &other)
        : m_runtime(other.m_runtime)
    {
    }
}