#ifndef GALAY_SOCKET_HPP
#define GALAY_SOCKET_HPP

#include "galay/common/Base.h"
#include "galay/common/Error.h"
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
    public:
        static AsyncTcpSocket create();
        static AsyncTcpSocket create(GHandle handle);

        AsyncTcpSocket();
        AsyncTcpSocket(GHandle handle);

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
        details::NetStatusContext m_ctx;
    };


    class AsyncUdpSocket
    {
    public:
        static AsyncUdpSocket create();
        static AsyncUdpSocket create(GHandle handle);

        AsyncUdpSocket();
        AsyncUdpSocket(GHandle handle);

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
        details::NetStatusContext m_ctx;
    };

    class AsyncSslSocket
    {
    public:
        static AsyncSslSocket create(SSL* ssl);
        static AsyncSslSocket create();

        AsyncSslSocket();
        AsyncSslSocket(SSL* ssl);

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
        GHandle getHandle() const;
    private:
        details::SslStatusContext m_ctx;
    };


}

#endif