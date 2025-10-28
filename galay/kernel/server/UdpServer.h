#ifndef GALAY_UDP_SERVER_H
#define GALAY_UDP_SERVER_H 

#include <condition_variable>
#include <functional>
#include <mutex>
#include "galay/kernel/async/Socket.h"
#include "galay/kernel/coroutine/Coroutine.hpp"

namespace galay
{
    /**
     * @brief UDP服务器类
     * @details 提供基于协程的异步UDP服务器实现，支持无连接数据报通信
     */
    class UdpServer
    { 
    public:
        /// 异步UDP处理函数类型：接收AsyncUdpSocket参数，返回Coroutine<nil>
        using AsyncUdpFunc = std::function<Coroutine<nil>(AsyncUdpSocket)>;
        
        /**
         * @brief 默认构造函数
         */
        UdpServer() {}
        
        /**
         * @brief 移动构造函数
         */
        UdpServer(UdpServer&& server);
        
        /**
         * @brief 删除的拷贝构造函数
         */
        UdpServer(const UdpServer& server) = delete;
        
        /**
         * @brief 监听指定的主机地址和端口
         * @param host 监听的主机地址（IP和端口）
         */
        void listenOn(Host host);
        
        /**
         * @brief 运行UDP服务器
         * @param runtime 运行时环境
         * @param callback 处理数据报的协程回调函数
         */
        void run(Runtime& runtime, const AsyncUdpFunc& callback);
        
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
        UdpServer& operator=(UdpServer&& server);
        
        /**
         * @brief 删除的拷贝赋值运算符
         */
        UdpServer& operator=(const UdpServer& server) = delete;
        
        ~UdpServer() {}
    protected:
        Host m_host = {"0.0.0.0", 8080};
        std::mutex m_mutex;
        std::condition_variable m_condition;
    };

    /**
     * @brief UDP服务器构建器
     * @details 使用构建器模式创建和配置UDP服务器
     */
    class UdpServerBuilder
    {
    public:
        /**
         * @brief 设置监听地址
         * @param host 监听的主机地址（IP和端口）
         * @return 构建器引用，支持链式调用
         */
        UdpServerBuilder& addListen(const Host& host);
        
        /**
         * @brief 构建UDP服务器
         * @return 配置完成的UdpServer对象
         */
        UdpServer build();
    private:
        Host m_host = {"0.0.0.0", 8080};  ///< 监听地址
    };
}

#endif