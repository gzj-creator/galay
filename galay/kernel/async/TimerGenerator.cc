#include "TimerGenerator.h"


namespace galay 
{
    TimerGenerator::TimerGenerator(Runtime& runtime)
    {
        RuntimeVisitor visitor(runtime);
        m_manager = visitor.timerManager().get();
    }

    AsyncResult<nil> TimerGenerator::sleepfor(std::chrono::milliseconds ms)
    {
        if(ms < std::chrono::milliseconds::zero()) {
            return {nil()};
        }
        return {std::make_shared<details::SleepforEvent>(m_manager, ms)};
    }
}