#include "TaskRunner.h"

namespace galay 
{
    TaskRunner::TaskRunner(Runtime &runtime, int co_id)
        : m_runtime(runtime), m_co_id(co_id)
    {
    }
}