#include "TaskRunner.h"
#include <cassert>

namespace galay 
{
    TaskRunner::TaskRunner(Runtime* runtime, int co_id)
        : m_runtime(runtime), m_co_id(co_id)
    {
        assert(m_runtime != nullptr && "Runtime pointer cannot be nullptr");
    }
}