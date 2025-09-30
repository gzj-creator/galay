#include "Thread.h"

namespace galay::thread
{
ThreadTask::ThreadTask(std::function<void()>&& func)
{
    this->m_func = func;
}

void 
ThreadTask::execute()
{
    this->m_func();
}

ThreadWaiters::ThreadWaiters(int num)
{
    this->m_num.store(num);
}

bool ThreadWaiters::wait(int timeout)
{
    std::unique_lock lock(this->m_mutex);
    if(m_num.load() == 0) return true;
    if(timeout == -1)
    {
        m_cond.wait(lock, [this]() {
            return m_num.load() == 0;
        });
    }
    else
    {
        m_cond.wait_for(lock, std::chrono::milliseconds(timeout), [this]() {
            return m_num.load() == 0;
        });
        if(m_num.load() != 0) return false;
    }
    return true;
}

bool ThreadWaiters::decrease()
{
    std::unique_lock lock(this->m_mutex);
    if( m_num.load() == 0) return false;
    m_num.fetch_sub(1);
    if(m_num.load() == 0)
    {
        m_cond.notify_one();
    }
    return true;
}

ScrambleThreadPool::ScrambleThreadPool(std::chrono::milliseconds timeout)
{
    m_stop.store(true, std::memory_order_relaxed);
    m_running.store(0);
    m_timeout = timeout;
}

void 
ScrambleThreadPool::run()
{
    std::chrono::milliseconds timeout = m_timeout;
    while (!m_stop.load())
    {
        ThreadTask::ptr task = nullptr;
        m_tasks.wait_dequeue_timed(task, timeout);
        if(task) task->execute();
    }
}

void 
ScrambleThreadPool::start(int num)
{
    if(!m_stop.load()) return;
    if(!m_threads.empty()) {
        m_threads.resize(0);
    }
    m_stop.store(false, std::memory_order_release);
    for (int i = 0; i < num; i++)
    {
        auto th = std::make_unique<std::thread>([this](){
            run();
            done();
        });
        m_threads.push_back(std::move(th));
    }
    m_running.store(num);
}

void 
ScrambleThreadPool::done()
{
    this->m_running.fetch_sub(1);
}

void 
ScrambleThreadPool::stop()
{
    if (!m_stop.load())
    {
        m_stop.store(true, std::memory_order_relaxed);
    }
    for(auto& thread : m_threads) {
        if(thread->joinable()) thread->join();
    }
}
}