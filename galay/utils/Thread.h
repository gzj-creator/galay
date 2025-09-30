#ifndef GALAY_THREAD_H
#define GALAY_THREAD_H

#include <thread>
#include <mutex>
#include <memory>
#include <shared_mutex>
#include <condition_variable>
#include <concurrentqueue/moodycamel/blockingconcurrentqueue.h>
#include <atomic>
#include <functional>
#include <future>

namespace galay::thread
{
class ThreadTask
{
public:
    using ptr = std::shared_ptr<ThreadTask>;
    ThreadTask(std::function<void()> &&func);
    void execute();
    ~ThreadTask() = default;
private:
    std::function<void()> m_func;
};

class ThreadWaiters
{
public:
    using ptr = std::shared_ptr<ThreadWaiters>;
    ThreadWaiters(int num);
    bool wait(int timeout = -1); //ms
    bool decrease();
private:
    std::mutex m_mutex;
    std::atomic_int m_num;
    std::condition_variable m_cond;
};

class ScrambleThreadPool
{
private:
    void run();
    void done();
public:
    using ptr = std::shared_ptr<ScrambleThreadPool>;
    using wptr = std::weak_ptr<ScrambleThreadPool>;
    using uptr = std::unique_ptr<ScrambleThreadPool>;
    
    ScrambleThreadPool(std::chrono::milliseconds timeout = std::chrono::milliseconds(50));
    template <typename F, typename... Args>
    inline auto addTask(F &&f, Args &&...args) -> std::future<decltype(f(args...))>;
    void start(int num);
    void stop();

protected:
    std::chrono::milliseconds m_timeout;
    moodycamel::BlockingConcurrentQueue<ThreadTask::ptr> m_tasks;  // 任务队列
    std::vector<std::unique_ptr<std::thread>> m_threads; // 工作线程
    std::atomic_uint8_t m_running;
    std::atomic_bool m_stop;   // 结束线程池
};

template <typename F, typename... Args>
inline auto ScrambleThreadPool::addTask(F &&f, Args &&...args) -> std::future<decltype(f(args...))>
{
    using RetType = decltype(f(args...));
    std::shared_ptr<std::packaged_task<RetType()>> func = std::make_shared<std::packaged_task<RetType()>>(std::bind(std::forward<F>(f), std::forward<Args>(args)...));
    auto t_func = [func]()
    {
        (*func)();
    };
    ThreadTask::ptr task = std::make_shared<ThreadTask>(t_func);
    m_tasks.enqueue(task);
    return func->get_future();
}

}

namespace galay::thread
{

template<typename T>
struct ListNode
{
    ListNode() 
        :m_prev(nullptr), m_next(nullptr) {}
    ListNode(const T& data)
        :m_prev(nullptr), m_next(nullptr), m_data(data)  {}
    ListNode(T&& data)
        :m_prev(nullptr), m_next(nullptr), m_data(std::forward<T>(data)) {}
    ListNode* m_prev;
    ListNode* m_next;
    T m_data;
};

template<typename T>
class List
{
    static_assert(std::is_default_constructible_v<T>, "T ust be default constructible");
public:
    List()
    {
        m_head = new ListNode<T>();
        m_tail = new ListNode<T>();
        m_head->m_next = m_tail;
        m_tail->m_prev = m_head;
        m_size.store(0);
    }

    ListNode<T>* pushFront(const T& data)
    {
        std::unique_lock lock(this->m_mtx);
        ListNode<T>* node = new ListNode<T>(data);
        node->m_next = m_head->m_next;
        node->m_prev = m_head;
        m_head->m_next->m_prev = node;
        m_head->m_next = node;
        m_size.fetch_add(1);
        return node;
    }

    ListNode<T>* pushFront(T&& data)
    {
        std::unique_lock lock(this->m_mtx);
        ListNode<T>* node = new ListNode<T>(std::forward<T>(data));
        node->m_next = m_head->m_next;
        node->m_prev = m_head;
        m_head->m_next->m_prev = node;
        m_head->m_next = node;
        m_size.fetch_add(1);
        return node;
    }

    ListNode<T>* pushBack(const T& data)
    {
        std::unique_lock lock(this->m_mtx);
        ListNode<T>* node = new ListNode<T>(data);
        node->m_next = m_tail;
        node->m_prev = m_tail->m_prev;
        m_tail->m_prev->m_next = node;
        m_tail->m_prev = node;
        m_size.fetch_add(1);
        return node;
    }

    ListNode<T>* pushBack(T&& data)
    {
        std::unique_lock lock(this->m_mtx);
        ListNode<T>* node = new ListNode<T>(std::forward<T>(data));
        node->m_next = m_tail;
        node->m_prev = m_tail->m_prev;
        m_tail->m_prev->m_next = node;
        m_tail->m_prev = node;
        m_size.fetch_add(1);
        return node;
    }

    ListNode<T>* popFront()
    {
        std::unique_lock lock(this->m_mtx);
        if( m_size.load() <= 0 ) return nullptr;
        ListNode<T>* node = m_head->m_next;
        node->m_next->m_prev = m_head;
        m_head->m_next = node->m_next;
        m_size.fetch_sub(1);
        node->m_next = nullptr;
        node->m_prev = nullptr;
        return node;
    }

    ListNode<T>* popBack()
    {
        std::unique_lock lock(this->m_mtx);
        if( m_size.load() == 0 ) return nullptr;
        ListNode<T>* node = m_tail->m_prev;
        node->m_prev->m_next = m_tail;
        m_tail->m_prev = node->m_prev;
        m_size.fetch_sub(1);
        node->m_next = nullptr;
        node->m_prev = nullptr;
        return node;
    }
    

    bool remove(ListNode<T>* node)
    {
        std::unique_lock lock(this->m_mtx);
        if( node == nullptr || node->m_next == nullptr || node->m_prev == nullptr ) return false;
        node->m_prev->m_next = node->m_next;
        node->m_next->m_prev = node->m_prev;
        m_size.fetch_sub(1);
        lock.unlock();
        delete node;
        return true;
    }

    bool empty()
    {
        return m_size.load() == 0;
    }
    
    uint64_t size()
    {
        return m_size.load();
    }

    ~List()
    {
        ListNode<T>* node = m_head;
        while(node)
        {
            ListNode<T>* temp = node;
            node = node->m_next;
            delete temp;
        }
    }
private:
    std::atomic_uint64_t m_size;
    ListNode<T>* m_head;
    ListNode<T>* m_tail;
    std::shared_mutex m_mtx;
};

}


#endif