#ifndef GALAY_SSL_EVENT_H
#define GALAY_SSL_EVENT_H 

#include "galay/kernel/coroutine/Result.hpp"
#include "galay/common/Log.h"
#include "galay/common/Common.h"
#include "Bytes.hpp"

namespace galay
{
    class AsyncSslSocket;
    class AsyncSslSocketBuilder {
        friend class AsyncTcpSslSocket;
    public:
        static AsyncSslSocketBuilder create(SSL* ssl);
        //throw exception
        AsyncSslSocket build();
        bool check() const;
    private:
        SSL* m_ssl = nullptr;
    };
 
}

namespace galay::details
{
    struct SslStatusContext {
        SSL* m_ssl = nullptr;
        bool m_is_connected = false;
        EventScheduler* m_scheduler = nullptr;
    };

    template <CoType T>
    class SslEvent: public AsyncEvent<T>
    {
    public:
        SslEvent(SslStatusContext& ctx): m_context(ctx) {}
        bool suspend(Waker waker) override {
            this->m_waker = waker;
            return true;
        }

        GHandle getHandle() override { return {SSL_get_fd(m_context.m_ssl)}; }
        bool setEventScheduler(EventScheduler* scheduler) override { m_context.m_scheduler = scheduler;  return true; }
        EventScheduler* belongEventScheduler() override { return m_context.m_scheduler; }
    protected:
        SslStatusContext& m_context;
    };

    class SslAcceptEvent: public SslEvent<ValueWrapper<AsyncSslSocketBuilder>>
    {
        enum class SslAcceptStatus: uint8_t
        {
            kSslAcceptStatus_Accept,
            kSslAcceptStatus_SslAccept,
        };
    public:
        SslAcceptEvent(SslStatusContext& ctx);
        std::string name() override { return "SslAcceptEvent"; }
        void handleEvent() override;
        EventType getEventType() const override;

        bool ready() override;
        bool suspend(Waker waker) override;
    private:
        bool sslAccept();
    private:
        SSL* m_accept_ssl;
        int m_ssl_code = 0;  
        SslAcceptStatus m_status;
    };

    class SslCloseEvent: public SslEvent<ValueWrapper<bool>> 
    {
    public:
        SslCloseEvent(SslStatusContext& ctx);
        std::string name() override { return "SslCloseEvent"; }
        void handleEvent() override;
        EventType getEventType() const override;

        bool ready() override;
        bool suspend(Waker waker) override;
    private:
        bool sslClose();
    private:
        int m_ssl_code = 0;
    };

    class SslConnectEvent: public SslEvent<ValueWrapper<bool>> 
    {
        enum class ConnectState {
            kConnectState_Ready,
            kConnectState_Connect,
            kConnectState_SslConnect,
        };
    public:
        SslConnectEvent(SslStatusContext& ctx, const Host& host);
        std::string name() override { return "SslConnectEvent"; }
        void handleEvent() override;
        EventType getEventType() const override;

        bool ready() override;
        bool suspend(Waker waker) override;
    private:
        bool sslConnect();
    private:
        Host m_host;
        int m_ssl_code = 0;
        ConnectState m_status;
    };

    class SslRecvEvent: public SslEvent<ValueWrapper<Bytes>> 
    {
    public:
        SslRecvEvent(SslStatusContext& ctx, size_t length);
        std::string name() override { return "SslRecvEvent"; }
        void handleEvent() override;
        EventType getEventType() const override { return kEventTypeRead; }

        bool ready() override;
        bool suspend(Waker waker) override;
    private:
        bool sslRecv();
    private:
        size_t m_length;
    };

    class SslSendEvent: public SslEvent<ValueWrapper<Bytes>> {
    public:
        SslSendEvent(SslStatusContext& ctx, Bytes&& bytes);
        std::string name() override { return "SslSendEvent"; }
        void handleEvent() override;
        EventType getEventType() const override { return kEventTypeWrite; }

        bool ready() override;
        bool suspend(Waker waker) override;
    private:
        bool sslSend();
    private:
        Bytes m_bytes;
    };



}


#endif