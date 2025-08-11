#include "ThreadPool.h"

namespace galay {
RoundRobinThreadIndexSelector::RoundRobinThreadIndexSelector(int max_thread_index)
    :m_balancer(createNodes(max_thread_index))
{
}

std::optional<int> RoundRobinThreadIndexSelector::select()
{
    return m_balancer.select();
}

std::vector<int> RoundRobinThreadIndexSelector::createNodes(int max_thread_index)
{
    std::vector<int> nodes(max_thread_index + 1);
    for (int i = 0; i <= max_thread_index; i++)
        nodes[i] = i;
    return nodes;
}

ScrambleThreadPool::ScrambleThreadPool(size_t size)
    : m_stop(true), m_threads(size)
{
}

void ScrambleThreadPool::start()
{
    if(m_stop.load() == false) return;
    m_stop = false;
    for(auto& t: m_threads) {
        t = std::thread([this]() {
            consumeQueue();
        });
    }
}

void ScrambleThreadPool::stop()
{
    if(m_stop.load() == true) return;
    m_stop = true;
    m_queue.enqueue(callback_t());
    for(auto& t: m_threads) {
        if(t.joinable()) t.join();
    }
}

void ScrambleThreadPool::consumeQueue()
{
    callback_t task;
    while(!m_stop) {
        m_queue.wait_dequeue(task);
        task();
    }
}

SpecifiedThreadPool::SpecifiedThreadPool(size_t size, ThreadIndexSelector::ptr selector)
    : m_stop(true), m_threads(size), m_queues(size), m_selector(selector)
{
    if(m_selector == nullptr) {
        m_selector = std::make_shared<RoundRobinThreadIndexSelector>(size - 1);
    }
}



void SpecifiedThreadPool::start()
{
    if(m_stop.load() == false) return;
    m_stop = false;
    for(int i = 0; i < m_threads.size(); ++i) {
        m_threads[i] = std::thread([this, i]() {
            consumeQueue(i);
        });
    }
}

void SpecifiedThreadPool::stop()
{
    if(m_stop.load() == true) return;
    m_stop = true;
    for(auto& q: m_queues) {
        q.enqueue(callback_t());
    }
    for(auto& t: m_threads) {
        if(t.joinable()) t.join();
    }
}

std::optional<int> SpecifiedThreadPool::allocateThreadIndex()
{
    return m_selector->select();
}


int SpecifiedThreadPool::size() const
{
    return m_queues.size();
}

void SpecifiedThreadPool::consumeQueue(int index)
{
    callback_t task;
    while(!m_stop) {
        m_queues[index].wait_dequeue(task);
        task();
    }
}


}