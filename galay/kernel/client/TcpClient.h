#ifndef GALAY_TCP_CLIENT_H
#define GALAY_TCP_CLIENT_H 

#include "galay/kernel/async/Socket.h"

namespace galay 
{
    /**
     * @brief TCP客户端类
     * @details 提供基于协程的异步TCP客户端实现，支持连接、发送和接收数据
     */
    class TcpClient 
    {
    public:
        /**
         * @brief 构造TCP客户端
         * @param runtime 运行时环境
         * @throw 可能抛出异常
         */
        TcpClient(Runtime& runtime);
        
        /**
         * @brief 从已有socket构造TCP客户端
         * @param socket 异步TCP socket（移动语义）
         */
        TcpClient(AsyncTcpSocket&& socket);
        
        /**
         * @brief 构造并绑定本地地址的TCP客户端
         * @param runtime 运行时环境
         * @param bind_addr 本地绑定地址
         */
        TcpClient(Runtime& runtime, const Host& bind_addr);
        
        /**
         * @brief 连接到远程服务器
         * @param addr 远程服务器地址
         * @return 异步结果，成功返回void，失败返回CommonError
         */
        AsyncResult<std::expected<void, CommonError>> connect(const Host& addr);
        
        /**
         * @brief 接收数据
         * @param buffer 接收缓冲区指针
         * @param length 期望接收的最大字节数
         * @return 异步结果，成功返回接收的Bytes，失败返回CommonError
         */
        AsyncResult<std::expected<Bytes, CommonError>> recv(char* buffer, size_t length);
        
        /**
         * @brief 发送数据
         * @param bytes 要发送的数据
         * @return 异步结果，成功返回void，失败返回CommonError
         */
        AsyncResult<std::expected<Bytes, CommonError>> send(Bytes bytes);
        
        /**
         * @brief 关闭连接
         * @return 异步结果，成功返回void，失败返回CommonError
         */
        AsyncResult<std::expected<void, CommonError>> close();
        
        /**
         * @brief 克隆客户端用于不同角色
         * @param runtime 新的运行时环境
         * @return 克隆的TcpClient对象
         */
        TcpClient cloneForDifferentRole(Runtime& runtime) const;
    private:
        AsyncTcpSocket m_socket;  ///< 异步TCP socket
    };
    
}

#endif