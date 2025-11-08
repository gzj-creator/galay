#include "TimerGenerator.h"
#include <cassert>

namespace galay 
{
    TimerGenerator::ptr TimerGenerator::createPtr(Runtime* runtime)
    {
        return std::make_shared<TimerGenerator>(runtime);
    }

    TimerGenerator::TimerGenerator(Runtime* runtime)
        :m_runtime(runtime)
    {
        assert(m_runtime != nullptr && "Runtime pointer cannot be nullptr");
    }

    AsyncResult<nil> TimerGenerator::wait(std::chrono::milliseconds ms, const std::function<void()> &func)
    {
        assert(m_runtime != nullptr && "Runtime pointer cannot be nullptr");
        if(ms < std::chrono::milliseconds::zero()) {
            return {nil()};
        }
        RuntimeVisitor visitor(*m_runtime);
        auto manager = visitor.timerManager().get();
        return {std::make_shared<details::TimeWaitEvent>(manager, ms, func)};
    }

    AsyncResult<nil> TimerGenerator::sleep(std::chrono::milliseconds ms)
    {
        assert(m_runtime != nullptr && "Runtime pointer cannot be nullptr");
        if(ms < std::chrono::milliseconds::zero()) {
            return {nil()};
        }
        RuntimeVisitor visitor(*m_runtime);
        auto manager = visitor.timerManager().get();
        return {std::make_shared<details::SleepforEvent>(manager, ms)};
    }
}