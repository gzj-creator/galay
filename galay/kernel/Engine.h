#ifndef GALAY_EVENT_ENGINE_H
#define GALAY_EVENT_ENGINE_H

#include "galay/common/Base.h"
#include "Waker.h"
#include "Timer.h"
#include <chrono>
#include <cstdint>
#include <atomic>
#include <concurrentqueue/moodycamel/concurrentqueue.h>
#include <functional>
#include <memory>

namespace galay
{

    class CoroutineBase;

    namespace details {
        template<typename ResultType>
        class NetResult;
    }

#define DEFAULT_MAX_EVENTS 1024

#if defined (USE_EPOLL)

#elif defined(USE_SELECT)

#elif defined(USE_KQUEUE)

    // Kqueue事件引擎 - 基于BSD kqueue机制的Reactor模式实现
    // 支持macOS和BSD系统，提供高效的I/O事件通知机制
    // default ET (Edge-Triggered) mode
    class KqueueEngine
    {
        template<typename ResultType>
        friend class details::NetResult;

    public:
        static constexpr int PollMaxTimeout = 100;
        static constexpr uint32_t BatchCoroutineDequeueSize = 1024;

        /**
         * @brief 构造函数
         * @param max_events 最大事件数量，默认为DEFAULT_MAX_EVENTS(1024)
         * 初始化kqueue实例和通知管道
         */
        explicit KqueueEngine(uint32_t max_events = DEFAULT_MAX_EVENTS);

        /**
         * @brief 启动事件引擎并等待事件
         * @param timeout 超时时间(毫秒)，-1表示无限等待，默认-1
         * @return 成功返回true，失败返回false
         * 注意：此方法只执行一次事件循环，持续运行需要在外部循环调用
         */
        bool start(int timeout = -1);

        /**
         * @brief 停止事件引擎
         * @return 成功返回true,失败为notify调用失败，getLastError查看错误
         * 设置停止标志并通过notify唤醒可能阻塞的事件循环
         */
        bool stop();

        /**
         * @brief 通知事件引擎唤醒
         * @return 成功返回true，失败返回false
         * 通过向通知管道写入数据来唤醒可能阻塞在kevent的线程
         */
        bool notify();

        /**
         * @brief 调度协程，最多PollMaxTimeout ms被调度
         * @return 成功返回true，notify失败返回false，getLastError查看错误
        */
        bool spawn(std::weak_ptr<CoroutineBase> co);

        /**
         * @brief 批量调度协程，最多PollMaxTimeout ms被调度
         * @param cos 协程弱引用列表
         * @return 成功返回true，失败返回false(某个或某些协程不可用或者notify调用失败)
         * 批量添加多个协程到就绪队列，减少notify调用次数，提高性能
         */
        bool spawnBatch(const std::vector<std::weak_ptr<CoroutineBase>>& cos);

        /**
         * @brief 检查引擎是否正在运行
         * @return 运行中返回true，否则返回false
         */
        bool isRunning() const { return !m_stop; }

        /**
         * @brief 获取最后的错误码
         * @return 错误码
         */
        uint64_t getLastError();

        /**
         * @brief 获取kqueue句柄
         * @return 返回kqueue的文件描述符
         */
        GHandle getHandle() { return m_handle; }

        /**
         * @brief 析构函数
         * 清理资源：关闭kqueue、通知管道、释放事件数组内存
         */
        ~KqueueEngine();
    protected:
        /**
         * @brief 添加Waker到事件引擎
         * @param wrapper Waker包装器，用于管理多种类型的Waker
         * @param type Waker类型(READ/WRITE/TIMER/FILE)
         * @param waker 实际的Waker对象
         * @param handle 文件句柄或事件标识
        * @param ctx 上下文指针，对于Timer事件传递超时毫秒数
        * @return 成功返回0，失败返回-1
        */
        int addWakers(WakerWrapper* wrapper, WakerType type, Waker waker, GHandle handle, void *ctx);

        /**
        * @brief 修改已存在的Waker
        * @param wrapper Waker包装器
        * @param type Waker类型
        * @param waker 新的Waker对象
        * @param handle 文件句柄或事件标识
        * @param ctx 上下文指针
        * @return 成功返回0，失败返回-1
        * 对于kqueue，修改操作和添加操作相同
        */
        int modWakers(WakerWrapper* wrapper, WakerType type, Waker waker, GHandle handle, void* ctx);

        /**
        * @brief 删除Waker
        * @param wrapper Waker包装器
        * @param type Waker类型
        * @param waker 要删除的Waker对象
        * @param handle 文件句柄或事件标识
        * @param ctx 上下文指针
        * @return 成功返回0，失败返回-1
        */
        int delWakers(WakerWrapper* wrapper, WakerType type, Waker waker, GHandle handle, void* ctx);
        
        /**
         * @brief 添加定时器
         * @param ms 定时器的毫秒间隔
         * @param callback 定时器回调函数
         * @return 定时器的智能指针
         *
         * 工作原理：
         * 1. 创建Timer对象并添加到TimerManager中
         * 2. 如果新添加的Timer是当前最早的定时器，则更新kqueue的timer监听超时时间
         * 3. 定时器触发时会执行回调函数，并重新调度下一个定时事件
         */
        Timer::ptr addTimer(std::chrono::milliseconds ms, const std::function<void()>& callback);

    private:
        void tick();
        bool convertToKEvent(struct kevent &ev, WakerType type, Waker* waker, GHandle handle, void* ctx);
    private:
        GHandle m_handle = GHandle::invalid();
        int m_notify_read_fd;
        int m_notify_write_fd;
        uint64_t m_last_error;
        std::atomic_bool m_stop;
        uint32_t m_event_size;
        struct kevent* m_events;
        moodycamel::ConcurrentQueue<std::weak_ptr<CoroutineBase>> m_ready_queue;

        TimerManager::ptr m_timer_manager;

        static constexpr int TIMER_IDEL = -1;  // 定时器标识符 
    };

    using Engine = KqueueEngine;
#endif





}

#endif