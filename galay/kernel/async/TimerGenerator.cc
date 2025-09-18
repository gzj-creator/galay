#include "TimerGenerator.h"


namespace galay 
{
    TimerGenerator::ptr TimerGenerator::createPtr(Runtime &runtime)
    {
        return std::make_shared<TimerGenerator>(runtime);
    }

    TimerGenerator::TimerGenerator(Runtime& runtime)
        :m_runtime(runtime), m_timer(std::make_shared<Timer>(std::chrono::milliseconds::zero(), nullptr))
    {
    }

    AsyncResult<nil> TimerGenerator::sleep(std::chrono::milliseconds ms)
    {
        if(ms < std::chrono::milliseconds::zero()) {
            return {nil()};
        }
        RuntimeVisitor visitor(m_runtime);
        auto manager = visitor.timerManager().get();
        return {std::make_shared<details::SleepforEvent>(manager, m_timer, ms)};
    }

    TimerGenerator::TimerGenerator(const TimerGenerator &other)
        : m_runtime(other.m_runtime), m_timer(std::make_shared<Timer>(std::chrono::milliseconds::zero(), nullptr))
    {
    }
}