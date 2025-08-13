#include "SinglePassGate.h"

namespace galay
{
    bool SinglePassGate::pass()
    {
        return !m_flag.test_and_set();
    }


}