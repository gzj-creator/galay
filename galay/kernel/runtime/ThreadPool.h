#ifndef GALAY_THREAD_POOL_H
#define GALAY_THREAD_POOL_H 

#include <functional>
#include <future>
#include "galay/common/Base.h"
#include "galay/common/Strategy.hpp"

namespace galay { 

class ScrambleThreadPool 
{
public:
    using callback_t = std::function<void()>;
    using ptr = std::shared_ptr<ScrambleThreadPool>;

    ScrambleThreadPool(size_t size);

    void start();
    void stop();

    template <typename F, typename... Args>
    auto addTask(F &&f, Args &&...args) -> std::future<decltype(f(args...))>
    {
        using RetType = decltype(f(args...));
        std::shared_ptr<std::packaged_task<RetType()>> func = std::make_shared<std::packaged_task<RetType()>>(std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        auto task = [func]()
        {
            (*func)();
        };
        if(m_queue.try_enqueue(task)) {
            return func->get_future();
        }
        m_queue.enqueue(task);
        return func->get_future();
    }

private:
    void consumeQueue();    

private:
    moodycamel::BlockingConcurrentQueue<callback_t> m_queue;

    std::atomic_bool m_stop;
    std::vector<std::thread> m_threads;
};

class ThreadIndexSelector
{
public:
    using ptr = std::shared_ptr<ThreadIndexSelector>;
    virtual std::optional<int> select() = 0;
};

class RoundRobinThreadIndexSelector: public ThreadIndexSelector
{ 
public:
    RoundRobinThreadIndexSelector(int max_thread_index);
    std::optional<int> select() override;
private:
    std::vector<int> createNodes(int max_thread_index);
private:
    details::RoundRobinLoadBalancer<int> m_balancer;
};

class SpecifiedThreadPool
{
public:
    using callback_t = std::function<void()>;
    SpecifiedThreadPool(size_t size, ThreadIndexSelector::ptr selector = nullptr);

    void start();
    void stop();

    std::optional<int> allocateThreadIndex();

    int size() const;

    template <typename F, typename... Args>
    auto addTask(int index, F &&f, Args &&...args) -> std::future<decltype(f(args...))>
    {
        using RetType = decltype(f(args...));
        std::shared_ptr<std::packaged_task<RetType()>> func = std::make_shared<std::packaged_task<RetType()>>(std::bind(std::forward<F>(f), std::forward<Args>(args)...));
        auto task = [func]()
        {
            (*func)();
        };
        if(m_queues[index].try_enqueue(task)) {
            return func->get_future();
        }
        m_queues[index].enqueue(task);
        return func->get_future();
    }

private:
    void consumeQueue(int index);

private:
    ThreadIndexSelector::ptr m_selector;
    std::atomic_bool m_stop;
    std::vector<std::thread> m_threads;
    std::vector<moodycamel::BlockingConcurrentQueue<callback_t>> m_queues;
};

}


#endif
