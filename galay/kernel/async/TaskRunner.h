#ifndef GALAY_TASK_RUNNER_H
#define GALAY_TASK_RUNNER_H 

#include "galay/kernel/runtime/Runtime.h"

namespace galay 
{ 
    class TaskRunner 
    {
    public:
        TaskRunner(Runtime& runtime, int co_id = -1);
        template<CoType T>
        void spawn(Coroutine<T>&& co);
    private:
        Runtime& m_runtime;
        int m_co_id;
        std::mutex m_mutex;
        std::condition_variable m_cond;
    };


    template <CoType T>
    inline void TaskRunner::spawn(Coroutine<T>&& co)
    {
        if(m_co_id != -1) m_runtime.schedule(std::move(co), m_co_id);
        else m_runtime.schedule(std::move(co));
    }
}

#endif