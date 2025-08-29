#include "SinglePassGate.h"

namespace galay
{
    SinglePassGate::SinglePassGate()
    {
    }

    bool SinglePassGate::pass()
    {
        return !m_flag.test_and_set();
    }

    SinglePassGate::~SinglePassGate()
    {
    }
}