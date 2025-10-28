#ifndef __GALAY_ERROR_H__
#define __GALAY_ERROR_H__

#include <string>

namespace galay::error
{
    /**
     * @brief 错误码枚举
     * @details 定义了Galay框架中所有可能的错误类型
     */
    enum ErrorCode
    {
        NoError,                          ///< 无错误
        DisConnectError,                  ///< 连接断开错误
        CallSocketError,                  ///< socket()调用错误
        CallBindError,                    ///< bind()调用错误
        CallListenError,                  ///< listen()调用错误
        CallAcceptError,                  ///< accept()调用错误
        CallConnectError,                 ///< connect()调用错误
        CallRecvError,                    ///< recv()调用错误
        CallRecvfromError,                ///< recvfrom()调用错误
        CallSendError,                    ///< send()调用错误
        CallSendtoError,                  ///< sendto()调用错误
        CallSendfileError,                ///< sendfile()调用错误
        CallShuntdownError,               ///< shutdown()调用错误
        CallCloseError,                   ///< close()调用错误
        CallSSLNewError,                  ///< SSL_new()调用错误
        CallSSLSetFdError,                ///< SSL_set_fd()调用错误
        CallSSLHandshakeError,            ///< SSL握手错误
        CallSSLShuntdownError,            ///< SSL_shutdown()调用错误
        CallSSLAcceptError,               ///< SSL_accept()调用错误
        CallSSLConnectError,              ///< SSL_connect()调用错误
        CallSSLCloseError,                ///< SSL关闭错误
        CallFileReadError,                ///< 文件读取错误
        CallFileWriteError,               ///< 文件写入错误
        CallLSeekError,                   ///< lseek()调用错误
        CallRemoveError,                  ///< remove()调用错误
        CallActiveEventError,             ///< 激活事件错误
        CallRemoveEventError,             ///< 移除事件错误
        CallGetSockNameError,             ///< getsockname()调用错误
        CallGetPeerNameError,             ///< getpeername()调用错误
        CallSetSockOptError,              ///< setsockopt()调用错误
        CallSetBlockError,                ///< 设置阻塞模式错误
        CallSetNoBlockError,              ///< 设置非阻塞模式错误
        CallInetNtopError,                ///< inet_ntop()调用错误
        CallEpollCreateError,             ///< epoll_create()调用错误
        CallEventWriteError,              ///< 事件写入错误
        CallKqueueCreateError,            ///< kqueue()创建错误
        CallOpenError,                    ///< open()调用错误
        CallAioSetupError,                ///< AIO设置错误
        CallAioSubmitError,               ///< AIO提交错误
        NotInitializedError,              ///< 未初始化错误
        AsyncTimeoutError,                ///< 异步超时错误
        NotifyButSourceNotReadyError,     ///< 通知时源未就绪错误
        FileReadEmptyError,               ///< 文件读取为空错误
        FileWriteEmptyError,              ///< 文件写入为空错误
        ConcurrentError,                  ///< 并发错误
        AioEventsAllCompleteError,        ///< AIO事件全部完成错误
        ErrorEnd                          ///< 错误枚举结束标记
    };

    /**
     * @brief 通用错误类
     * @details 封装Galay错误码和系统错误码，提供错误信息查询功能
     */
    class CommonError
    {
    public:
        /**
         * @brief 检查错误码是否包含指定错误类型
         * @param error 错误码
         * @param code 要检查的错误类型
         * @return 是否包含该错误类型
         */
        static bool contains(uint64_t error, ErrorCode code);
        
        /**
         * @brief 构造错误对象
         * @param galay_code Galay框架错误码
         * @param system_code 系统错误码（如errno）
         */
        CommonError(uint32_t galay_code, uint32_t system_code);
        
        /**
         * @brief 获取组合后的错误码
         * @return 64位错误码（高32位为Galay码，低32位为系统码）
         */
        uint64_t code() const;
        
        /**
         * @brief 获取错误消息字符串
         * @return 可读的错误描述
         */
        std::string message() const;
        
        /**
         * @brief 重置错误为无错误状态
         */
        void reset();
    private:
        /**
         * @brief 将两个32位错误码组合成64位
         */
        uint64_t makeErrorCode(uint32_t galay_code, uint32_t system_code);
    private:
        uint64_t m_code;  ///< 组合后的错误码
    };

    /**
     * @brief 无错误类型
     * @details 用于表示不会失败的操作
     */
    class Infallible
    {
    };

}

#endif