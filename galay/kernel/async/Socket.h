#ifndef GALAY_SOCKET_HPP
#define GALAY_SOCKET_HPP

#include "galay/common/Base.h"
#include "galay/common/Error.h"
#include "galay/kernel/runtime/Runtime.h"
#include "NetEvent.h"
#include "SslEvent.h"

namespace galay {

    /*
    *************************************************
                net(not thread security)
    *************************************************
    */

    class AsyncTcpSocket
    {
        friend class AsyncTcpSocketBuilder;
    public:
        AsyncTcpSocket(Runtime& runtime);
        AsyncTcpSocket(Runtime& runtime, GHandle handle);

        HandleOption options();

        ValueWrapper<bool> socket();
        ValueWrapper<bool> bind(const Host& addr);
        ValueWrapper<bool> listen(int backlog);
        AsyncResult<ValueWrapper<bool>> close();
        //throw exception
        AsyncResult<ValueWrapper<AsyncTcpSocketBuilder>> accept();
        AsyncResult<ValueWrapper<bool>> connect(const Host& addr);
        AsyncResult<ValueWrapper<Bytes>> recv(size_t length);
        //返回剩余Bytes
        AsyncResult<ValueWrapper<Bytes>> send(Bytes bytes);
        
    #ifdef __linux__
        //return
        AsyncResult<ValueWrapper<bool>> sendfile(GHandle file_handle, long offset, size_t length);
    #endif
        //throw exception
        [[nodiscard]] ValueWrapper<SockAddr> getSrcAddr() const;
        //throw exception
        [[nodiscard]] ValueWrapper<SockAddr> getDestAddr() const;
    private:
        AsyncTcpSocket(EventScheduler* scheduler, GHandle handle);
    private:
        GHandle m_handle;
        EventScheduler* m_scheduler = nullptr;
    };


    class AsyncUdpSocket
    {
    public:
        AsyncUdpSocket(Runtime& runtime);
        AsyncUdpSocket(Runtime& runtime, GHandle handle);

        HandleOption options();
        ValueWrapper<bool> socket();
        ValueWrapper<bool> bind(const Host& addr);
        ValueWrapper<bool> connect(const Host& addr);
        AsyncResult<ValueWrapper<Bytes>> recvfrom(Host& remote, size_t length);
        AsyncResult<ValueWrapper<Bytes>> sendto(const Host& remote, Bytes bytes);
        AsyncResult<ValueWrapper<bool>> close();

        //throw exception
        [[nodiscard]] ValueWrapper<SockAddr> getSrcAddr() const;
        //throw exception
        [[nodiscard]] ValueWrapper<SockAddr> getDestAddr() const;
    private:
        GHandle m_handle;
        EventScheduler* m_scheduler = nullptr;
    };

    class AsyncSslSocket
    {
        friend class AsyncSslSocketBuilder;
    public:
        AsyncSslSocket(Runtime& runtime);
        AsyncSslSocket(Runtime& runtime, SSL* ssl);

        HandleOption options();
        ValueWrapper<bool> socket();
        ValueWrapper<bool> bind(const Host& addr);
        ValueWrapper<bool> listen(int backlog);
        AsyncResult<ValueWrapper<AsyncSslSocketBuilder>> sslAccept();
        AsyncResult<ValueWrapper<bool>> sslConnect(const Host& addr);
        AsyncResult<ValueWrapper<Bytes>> sslRecv(size_t length);
        AsyncResult<ValueWrapper<Bytes>> sslSend(Bytes bytes);
        AsyncResult<ValueWrapper<bool>> sslClose();
    private:
        AsyncSslSocket(EventScheduler* scheduler, SSL* ssl);
        GHandle getHandle() const;
    private:
        SSL* m_ssl = nullptr;
        EventScheduler* m_scheduler = nullptr;
    };


}

#endif