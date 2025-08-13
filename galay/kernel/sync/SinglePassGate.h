#ifndef GALAY_SINGLE_PASS_GATE_H
#define GALAY_SINGLE_PASS_GATE_H 

#include <atomic>
#include <memory>

namespace galay
{
    class SinglePassGate 
    {
    public:
        using ptr = std::shared_ptr<SinglePassGate>;
        bool pass();
    private:
        std::atomic_flag m_flag;
    };
}

#endif