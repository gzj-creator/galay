#ifndef GALAY_KERNEL_TCP_SERVER_H
#define GALAY_KERNEL_TCP_SERVER_H 

#include <condition_variable>
#include <functional>
#include <mutex>
#include <vector>
#include "galay/kernel/async/Socket.h"
#include "galay/kernel/coroutine/Coroutine.hpp"
#include "ServerDefn.hpp"

namespace galay 
{

    /**
     * @brief TCP服务器类
     * @details 提供基于协程的异步TCP服务器实现，支持多客户端并发连接
     */
    class TcpServer
    {
    public:
        /// 异步TCP处理函数类型：接收AsyncTcpSocket参数，返回Coroutine<nil>
        using AsyncTcpFunc = std::function<Coroutine<nil>(AsyncTcpSocket, CoSchedulerHandle)>;
        
        /**
         * @brief 默认构造函数
         */
        TcpServer() {}
        
        /**
         * @brief 移动构造函数
         */
        TcpServer(TcpServer&& server);
        
        /**
         * @brief 删除的拷贝构造函数
         */
        TcpServer(const TcpServer& server) = delete;
        
        /**
         * @brief 监听指定的主机地址和端口
         * @param host 监听的主机地址（IP和端口）
         * @param backlog 监听队列的最大长度
         */
        void listenOn(Host host, int backlog);
        
        /**
         * @brief 运行TCP服务器
         * @param runtime 运行时环境
         * @param callback 处理新连接的协程回调函数
         * @return 启动是否成功
         */
        bool run(Runtime& runtime, const AsyncTcpFunc& callback);
        
        /**
         * @brief 停止服务器
         */
        void stop();
        
        /**
         * @brief 等待服务器停止
         */
        void wait();
        
        /**
         * @brief 移动赋值运算符
         */
        TcpServer& operator=(TcpServer&& server);
        
        /**
         * @brief 删除的拷贝赋值运算符
         */
        TcpServer& operator=(const TcpServer& server) = delete;
        
        ~TcpServer() {}
    private:
        Coroutine<nil> acceptConnection(CoSchedulerHandle handle, AsyncTcpFunc callback, size_t i);
    protected:
        int m_backlog = DEFAULT_TCP_BACKLOG_SIZE;
        Host m_host = {"0.0.0.0", 8080};
        std::mutex m_mutex;
        std::condition_variable m_condition;
        std::vector<AsyncTcpSocket> m_sockets;
        std::atomic<bool> m_running = false;
    };

    

    /**
     * @brief TCP服务器构建器
     * @details 使用构建器模式创建和配置TCP服务器
     */
    class TcpServerBuilder
    {
    public:
        /**
         * @brief 设置监听地址
         * @param host 监听的主机地址（IP和端口）
         * @return 构建器引用，支持链式调用
         */
        TcpServerBuilder& addListen(const Host& host);
        
        /**
         * @brief 设置监听队列长度
         * @param backlog 监听队列最大长度
         * @return 构建器引用，支持链式调用
         */
        TcpServerBuilder& backlog(int backlog);
        
        /**
         * @brief 构建TCP服务器
         * @return 配置完成的TcpServer对象
         */
        TcpServer build();
    private:
        Host m_host = {"0.0.0.0", 8080};             ///< 监听地址
        int m_backlog = DEFAULT_TCP_BACKLOG_SIZE;    ///< 监听队列长度
    };


}


#endif