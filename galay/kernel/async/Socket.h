#ifndef GALAY_SOCKET_HPP
#define GALAY_SOCKET_HPP

#include "galay/common/Base.h"
#include "galay/common/Error.h"
#include "galay/kernel/runtime/Runtime.h"
#include "NetEvent.h"
#include "SslEvent.h"

namespace galay {

    #define DEFAULT_BUFFER_SIZE 1024

    using namespace error;
    /*
    *************************************************
                net(not thread security)
    *************************************************
    */
    enum class ShutdownType {
        Read = 0,
        Write = 1,
        Both = 2
    };

    class AsyncTcpSocket
    {
        friend class AsyncTcpSocketBuilder;
    public:
        AsyncTcpSocket() = default;
        AsyncTcpSocket(Runtime* runtime);
        AsyncTcpSocket(Runtime* runtime, GHandle handle);
        AsyncTcpSocket(const AsyncTcpSocket& other) = delete;
        AsyncTcpSocket(AsyncTcpSocket&& other);
        AsyncTcpSocket& operator=(const AsyncTcpSocket& other) = delete;
        AsyncTcpSocket& operator=(AsyncTcpSocket&& other);
        ~AsyncTcpSocket();
        HandleOption options();

        std::expected<void, CommonError> socket();
        std::expected<void, CommonError> bind(const Host& addr);
        std::expected<void, CommonError> listen(int backlog);
        std::expected<void, CommonError> shuntdown(ShutdownType type);
        AsyncResult<std::expected<void, CommonError>> close();
        //throw exception
        AsyncResult<std::expected<void, CommonError>> accept(AsyncTcpSocketBuilder& builder);
        AsyncResult<std::expected<void, CommonError>> connect(const Host& addr);
        AsyncResult<std::expected<Bytes, CommonError>> recv(char* result, size_t length);
        //返回剩余Bytes
        AsyncResult<std::expected<Bytes, CommonError>> send(Bytes bytes);
        
    #ifdef __linux__
        //return send length 
        AsyncResult<std::expected<long, CommonError>> sendfile(GHandle file_handle, long offset, size_t length);
    #endif
        //throw exception
        [[nodiscard]] std::expected<SockAddr, CommonError> getSrcAddr() const;
        //throw exception
        [[nodiscard]] std::expected<SockAddr, CommonError> getDestAddr() const;

        AsyncTcpSocket cloneForDifferentRole(Runtime* runtime) const;
    private:
        AsyncTcpSocket(EventScheduler* scheduler, GHandle handle);
    private:
        GHandle m_handle;
        EventScheduler* m_scheduler = nullptr;
    };


    class AsyncUdpSocket
    {
    public:
        AsyncUdpSocket() = default;
        AsyncUdpSocket(Runtime* runtime);
        AsyncUdpSocket(Runtime* runtime, GHandle handle);

        AsyncUdpSocket(const AsyncUdpSocket& other) = delete;
        AsyncUdpSocket(AsyncUdpSocket&& other);
        AsyncUdpSocket& operator=(const AsyncUdpSocket& other) = delete;
        AsyncUdpSocket& operator=(AsyncUdpSocket&& other);
        ~AsyncUdpSocket();

        HandleOption options();
        std::expected<void, CommonError> socket();
        std::expected<void, CommonError> bind(const Host& addr);
        std::expected<void, CommonError> connect(const Host& addr);
        AsyncResult<std::expected<Bytes, CommonError>> recv(char* result, size_t length);
        AsyncResult<std::expected<Bytes, CommonError>> send(Bytes bytes);
        AsyncResult<std::expected<Bytes, CommonError>> recvfrom(Host& remote, char* result, size_t length);
        AsyncResult<std::expected<Bytes, CommonError>> sendto(const Host& remote, Bytes bytes);
        AsyncResult<std::expected<void, CommonError>> close();
        //throw exception
        [[nodiscard]] std::expected<SockAddr, CommonError> getSrcAddr() const;
        //throw exception
        [[nodiscard]] std::expected<SockAddr, CommonError> getDestAddr() const;
        AsyncUdpSocket cloneForDifferentRole(Runtime* runtime) const;
    private:
        GHandle m_handle;
        EventScheduler* m_scheduler = nullptr;
    };

    class AsyncSslSocket
    {
        friend class AsyncSslSocketBuilder;
    public:
        AsyncSslSocket() = default;
        AsyncSslSocket(Runtime* runtime, SSL_CTX* ssl_ctx);
        AsyncSslSocket(Runtime* runtime, SSL* ssl);
        AsyncSslSocket(AsyncSslSocket&& other);
        AsyncSslSocket(const AsyncSslSocket& other) = delete;
        AsyncSslSocket& operator=(const AsyncSslSocket& other) = delete;
        AsyncSslSocket& operator=(AsyncSslSocket&& other);
        HandleOption options();
        std::expected<void, CommonError> socket();
        std::expected<void, CommonError> bind(const Host& addr);
        std::expected<void, CommonError> listen(int backlog);
        AsyncResult<std::expected<void, CommonError>> accept(AsyncSslSocketBuilder& builder);
        std::expected<void, CommonError> readyToSslAccept(AsyncSslSocketBuilder &builder);
        //false need to be called again
        //CallSSLHandshakeError 失败AsyncSslSocketBuilder不可用
        AsyncResult<std::expected<bool, CommonError>> sslAccept(AsyncSslSocketBuilder& builder);
        AsyncResult<std::expected<void, CommonError>> connect(const Host& addr);
        void readyToSslConnect();
        //false need to be called again
        AsyncResult<std::expected<bool, CommonError>> sslConnect();
        AsyncResult<std::expected<Bytes, CommonError>> sslRecv(char* result, size_t length);
        AsyncResult<std::expected<Bytes, CommonError>> sslSend(Bytes bytes);
        AsyncResult<std::expected<void, CommonError>> sslClose();
        AsyncSslSocket cloneForDifferentRole(Runtime* runtime) const;
        SSL* getSsl() const;
        ~AsyncSslSocket();
    private:
        AsyncSslSocket(EventScheduler* scheduler, SSL* ssl);
        GHandle getHandle() const;
    private:
        SSL* m_ssl = nullptr;
        EventScheduler* m_scheduler = nullptr;
    };


}

#endif