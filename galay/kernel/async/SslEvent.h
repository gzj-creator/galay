#ifndef GALAY_SSL_EVENT_H
#define GALAY_SSL_EVENT_H 

#include "galay/kernel/coroutine/Result.hpp"
#include "galay/common/Common.h"
#include "Bytes.h"

namespace galay
{
    class AsyncSslSocket;
    class AsyncSslSocketBuilder {
        friend class AsyncTcpSslSocket;
    public:
        static AsyncSslSocketBuilder create(EventScheduler* scheduler, SSL* ssl);
        //throw exception
        AsyncSslSocket build();
        bool check() const;
    private:
        SSL* m_ssl = nullptr;
        EventScheduler* m_scheduler = nullptr;
    };
 
}

namespace galay::details
{
    template <CoType T>
    class SslEvent: public AsyncEvent<T>, public Event
    {
    public:
        SslEvent(SSL* ssl, EventScheduler* scheduler)
            : m_ssl(ssl), m_scheduler(scheduler) {}
    
        GHandle getHandle() override { 
            if(m_ssl) {
                return {SSL_get_fd(m_ssl)};
            }
            return GHandle{};
        }

        bool suspend(Waker waker) override {
            using namespace error;
            this->m_waker = waker;
            if(!m_scheduler->activeEvent(this, nullptr)) {
                Error::ptr error = std::make_shared<SystemError>(ErrorCode::CallActiveEventError, errno);
                makeValue(this->m_result, error);
                return false;
            }
            return true;
        }
    protected:
        SSL* m_ssl;
        EventScheduler* m_scheduler;
    };

    class SslAcceptEvent: public SslEvent<ValueWrapper<AsyncSslSocketBuilder>>
    {
        enum class SslAcceptStatus: uint8_t
        {
            kSslAcceptStatus_Accept,
            kSslAcceptStatus_SslAccept,
        };
    public:
        SslAcceptEvent(SSL* ssl, EventScheduler* scheduler);
        std::string name() override { return "SslAcceptEvent"; }
        void handleEvent() override;
        EventType getEventType() const override;

        bool ready() override;
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
        SslCloseEvent(SSL* ssl, EventScheduler* scheduler);
        std::string name() override { return "SslCloseEvent"; }
        void handleEvent() override;
        EventType getEventType() const override;
        bool ready() override;
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
        SslConnectEvent(SSL* ssl, EventScheduler* scheduler, const Host& host);
        std::string name() override { return "SslConnectEvent"; }
        void handleEvent() override;
        EventType getEventType() const override;

        bool ready() override;
        
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
        SslRecvEvent(SSL* ssl, EventScheduler* scheduler, StringMetaData& data, size_t length);
        std::string name() override { return "SslRecvEvent"; }
        void handleEvent() override;
        EventType getEventType() const override { return kEventTypeRead; }
        bool ready() override;
    private:
        bool sslRecv();
    private:
        size_t m_length;
        StringMetaData& m_data;
    };

    class SslSendEvent: public SslEvent<ValueWrapper<Bytes>> {
    public:
        SslSendEvent(SSL* ssl, EventScheduler* scheduler, Bytes&& bytes);
        std::string name() override { return "SslSendEvent"; }
        void handleEvent() override;
        EventType getEventType() const override { return kEventTypeWrite; }
        bool ready() override;
    private:
        bool sslSend();
    private:
        Bytes m_bytes;
    };



}


#endif