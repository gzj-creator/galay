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
        using wptr = std::weak_ptr<SinglePassGate>;
        SinglePassGate();
        bool pass();
        SinglePassGate(const SinglePassGate&) = delete;
        SinglePassGate(SinglePassGate&&) = delete;
        SinglePassGate& operator=(const SinglePassGate&) = delete;
        SinglePassGate& operator=(SinglePassGate&&) = delete;
        ~SinglePassGate();
    private:
        std::atomic_flag m_flag;
    };
}

#endif