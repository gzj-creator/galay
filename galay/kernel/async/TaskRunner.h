#ifndef GALAY_TASK_RUNNER_H
#define GALAY_TASK_RUNNER_H 

#include "galay/kernel/runtime/Runtime.h"
#include <cassert>

namespace galay 
{ 
    class TaskRunner: public std::enable_shared_from_this<TaskRunner>
    {
    public:
        TaskRunner() = default;
        TaskRunner(Runtime* runtime, int co_id = -1);
        template<CoType T>
        void spawn(Coroutine<T>&& co);
    private:
        Runtime* m_runtime = nullptr;
        int m_co_id = -1;
    };


    template <CoType T>
    inline void TaskRunner::spawn(Coroutine<T>&& co)
    {
        assert(m_runtime != nullptr && "Runtime pointer cannot be nullptr");
        if(m_co_id != -1) m_runtime->schedule(std::move(co), m_co_id);
        else m_runtime->schedule(std::move(co));
    }
}

#endif