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
    /**
    * @brief 线程任务类
    * @details 封装需要在线程池中执行的任务函数
    */
    class ThreadTask
    {
    public:
        using ptr = std::shared_ptr<ThreadTask>;
        
        /**
        * @brief 构造线程任务
        * @param func 任务函数（移动语义）
        */
        ThreadTask(std::function<void()> &&func);
        
        /**
        * @brief 执行任务
        */
        void execute();
        
        ~ThreadTask() = default;
    private:
        std::function<void()> m_func;  ///< 任务函数
    };

    /**
    * @brief 线程等待器
    * @details 用于等待多个异步任务完成，支持超时控制
    */
    class ThreadWaiters
    {
    public:
        using ptr = std::shared_ptr<ThreadWaiters>;
        
        /**
        * @brief 构造等待器
        * @param num 需要等待的任务数量
        */
        ThreadWaiters(int num);
        
        /**
        * @brief 等待所有任务完成
        * @param timeout 超时时间（毫秒），-1表示无限等待
        * @return 是否在超时前完成所有任务
        */
        bool wait(int timeout = -1);
        
        /**
        * @brief 减少一个等待计数
        * @return 操作是否成功
        */
        bool decrease();
    private:
        std::mutex m_mutex;
        std::atomic_int m_num;
        std::condition_variable m_cond;
    };

    /**
    * @brief 抢占式线程池
    * @details 高性能线程池实现，使用无锁并发队列，支持多个工作线程竞争执行任务
    */
    class ScrambleThreadPool
    {
    private:
        void run();
        void done();
    public:
        using ptr = std::shared_ptr<ScrambleThreadPool>;
        using wptr = std::weak_ptr<ScrambleThreadPool>;
        using uptr = std::unique_ptr<ScrambleThreadPool>;
        
        /**
        * @brief 构造线程池
        * @param timeout 任务队列出队超时时间，默认50毫秒
        */
        ScrambleThreadPool(std::chrono::milliseconds timeout = std::chrono::milliseconds(50));
        
        /**
        * @brief 添加任务到线程池
        * @tparam F 函数类型
        * @tparam Args 参数类型
        * @param f 任务函数
        * @param args 函数参数
        * @return std::future对象，用于获取任务结果
        */
        template <typename F, typename... Args>
        inline auto addTask(F &&f, Args &&...args) -> std::future<decltype(f(args...))>;
        
        /**
        * @brief 启动线程池
        * @param num 工作线程数量
        */
        void start(int num);
        
        /**
        * @brief 停止线程池
        */
        void stop();

    protected:
        std::chrono::milliseconds m_timeout;                                    ///< 超时时间
        moodycamel::BlockingConcurrentQueue<ThreadTask::ptr> m_tasks;          ///< 任务队列
        std::vector<std::unique_ptr<std::thread>> m_threads;                   ///< 工作线程
        std::atomic_uint8_t m_running;                                          ///< 运行状态
        std::atomic_bool m_stop;                                                ///< 停止标志
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

    /**
    * @brief 链表节点结构
    * @tparam T 数据类型
    */
    template<typename T>
    struct ListNode
    {
        ListNode() 
            :m_prev(nullptr), m_next(nullptr) {}
        ListNode(const T& data)
            :m_prev(nullptr), m_next(nullptr), m_data(data)  {}
        ListNode(T&& data)
            :m_prev(nullptr), m_next(nullptr), m_data(std::forward<T>(data)) {}
        ListNode* m_prev;  ///< 前驱节点
        ListNode* m_next;  ///< 后继节点
        T m_data;          ///< 数据
    };

    /**
    * @brief 线程安全的双向链表
    * @tparam T 数据类型（必须是默认可构造的）
    * @details 提供线程安全的双向链表实现，支持前后端插入和删除操作
    */
    template<typename T>
    class List
    {
        static_assert(std::is_default_constructible_v<T>, "T ust be default constructible");
    public:
        /**
        * @brief 构造空链表
        */
        List()
        {
            m_head = new ListNode<T>();
            m_tail = new ListNode<T>();
            m_head->m_next = m_tail;
            m_tail->m_prev = m_head;
            m_size.store(0);
        }

        /**
        * @brief 在链表头部插入元素
        * @param data 要插入的数据
        * @return 新插入节点的指针
        */
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

        /**
        * @brief 从链表头部移除元素
        * @return 移除节点的指针，若链表为空则返回nullptr
        */
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
        

        /**
        * @brief 移除指定节点
        * @param node 要移除的节点指针
        * @return 操作是否成功
        */
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

        /**
        * @brief 检查链表是否为空
        * @return 是否为空
        */
        bool empty()
        {
            return m_size.load() == 0;
        }
        
        /**
        * @brief 获取链表大小
        * @return 节点数量
        */
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