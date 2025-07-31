#include "RateLimiter.h"
#include <functional>

#include <assert.h>

namespace galay::utils 
{
CountSemaphore::CountSemaphore(uint64_t initcount,uint64_t capacity)
{
    assert(initcount <= capacity);
    m_capacity = capacity;
    m_nowcount = initcount;
}

bool 
CountSemaphore::get(uint64_t count)
{
    std::unique_lock<std::mutex> lock(m_mtx);
    if(count > m_capacity) return false;
    m_cond.wait(lock,[this,count](){
        return m_nowcount >= count;
    });
    m_nowcount -= count;
    return true;
}


void 
CountSemaphore::put(uint64_t count)
{
    std::unique_lock<std::mutex> lock(m_mtx);
    //可以m_nowcount += count，因为Get和Put互斥，+=时Get无法进入，不存在m_nowcount > m_capacity时大流量包的误判
    m_nowcount += count;
    if(m_nowcount > m_capacity) m_nowcount = m_capacity;
    lock.unlock();
    m_cond.notify_all();
}


RateLimiter::RateLimiter(uint64_t rate, uint64_t capacity,uint64_t deliveryInteralMs)
{
    this->m_rate = rate;
    this->m_semaphore = std::make_unique<CountSemaphore>(capacity,capacity);
    this->m_deliveryInteralMs = deliveryInteralMs;
    this->m_deliveryThread = nullptr;
    this->m_runing = false;
}

void
RateLimiter::start()
{
    if(m_runing) return;
    m_runing = true;
    m_deliveryThread = std::make_unique<std::thread>(std::bind(&RateLimiter::produceToken,this));
}

void 
RateLimiter::stop()
{
    m_runing = false;
    if(m_deliveryThread) {
        if(m_deliveryThread->joinable()) m_deliveryThread->join();
    }
}

void 
RateLimiter::produceToken()
{
    auto lastTime = std::chrono::steady_clock::now();
    while(m_runing)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(m_deliveryInteralMs));
        auto curTime = std::chrono::steady_clock::now();
        auto elapseMs = std::chrono::duration<double,std::milli>(curTime - lastTime).count();
        lastTime = curTime;
        auto tokens = elapseMs * m_rate / 1000;
        m_semaphore->put(static_cast<uint64_t>(tokens));
    }
}

bool
RateLimiter::pass(uint64_t flow)
{
    return m_semaphore->get(flow);
}

RateLimiter::~RateLimiter()
{
    m_deliveryThread.reset(nullptr);
}

}