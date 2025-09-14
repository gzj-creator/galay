#include "TimerGenerator.h"


namespace galay 
{
    TimerGenerator::ptr TimerGenerator::createPtr(Runtime &runtime, int co_id)
    {
        return std::make_shared<TimerGenerator>(runtime, co_id);
    }

    TimerGenerator::TimerGenerator(Runtime& runtime, int co_id)
        :m_runtime(runtime), m_timer(std::make_shared<Timer>(std::chrono::milliseconds::zero(), nullptr)), m_co_id(co_id)
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