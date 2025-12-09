#ifndef GALAY_SSL_EVENT_H
#define GALAY_SSL_EVENT_H 

#include "galay/kernel/coroutine/Result.hpp"
#include "galay/common/Common.h"
#include "Bytes.h"

namespace galay
{
    class AsyncSslSocket;
    class AsyncSslSocketBuilder {
        friend class AsyncSslSocket;
    public:
        AsyncSslSocketBuilder(SSL_CTX* ssl_ctx) : m_ssl_ctx(ssl_ctx) {}
        //throw exception
        AsyncSslSocket build();
        bool check() const;
    private:
        SSL_CTX* m_ssl_ctx = nullptr;
        GHandle m_handle = GHandle::invalid();
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
        GHandle getHandle() override { 
            if(m_ssl) {
                return {SSL_get_fd(m_ssl)};
            }
            return GHandle{};
        }

        bool onSuspend(Waker waker) override {
            using namespace error;
            this->m_waker = waker;
            if(!m_scheduler->activeEvent(this, nullptr)) {
                this->m_result = std::unexpected(CommonError(CallActiveEventError, static_cast<uint32_t>(errno)));
                return false;
            }
            return true;
        }
    protected:
        SSL* m_ssl;
        EventScheduler* m_scheduler;
    };

    class SslAcceptEvent: public SslEvent<std::expected<bool, CommonError>>
    {
    public:
        
        void reset(SSL* ssl, EventScheduler* scheduler) {
            m_ssl = ssl;
            m_scheduler = scheduler;
            m_ready = false;
            m_ssl_code = 0;
        }

        std::string name() override { return "SslAcceptEvent"; }
        void handleEvent() override;
        EventType getEventType() const override;
        bool onReady() override;
        std::expected<bool, CommonError> onResume() override;
    private:
        bool sslAccept(bool notify);
    private:
        bool m_ready = false;
        int m_ssl_code = 0;
    };

    class SslCloseEvent: public SslEvent<std::expected<void, CommonError>>
    {
    public:
        void reset(SSL* ssl, EventScheduler* scheduler) {
            m_ssl = ssl;
            m_scheduler = scheduler;
            m_ssl_code = 0;
        }

        std::string name() override { return "SslCloseEvent"; }
        void handleEvent() override;
        EventType getEventType() const override;
        bool onReady() override;
    private:
        bool sslClose();
    private:
        int m_ssl_code = 0;
    };

    class SslConnectEvent: public SslEvent<std::expected<bool, CommonError>>
    {
    public:
        void reset(SSL* ssl, EventScheduler* scheduler) {
            m_ssl = ssl;
            m_scheduler = scheduler;
            m_ready = false;
            m_ssl_code = 0;
        }

        std::string name() override { return "SslConnectEvent"; }
        void handleEvent() override;
        EventType getEventType() const override;
        bool onReady() override;
        std::expected<bool, CommonError> onResume() override;
    private:
        bool sslConnect(bool notify);
    private:
        bool m_ready = false;
        int m_ssl_code = 0;
    };

    class SslRecvEvent: public SslEvent<std::expected<Bytes, CommonError>>
    {
    public:
        void reset(SSL* ssl, EventScheduler* scheduler, char* result, size_t length) {
            m_ssl = ssl;
            m_scheduler = scheduler;
            m_result_str = result;
            m_length = length;
            m_ready = false;
        }

        std::string name() override { return "SslRecvEvent"; }
        void handleEvent() override;
        EventType getEventType() const override { return kEventTypeRead; }
        bool onReady() override;
        std::expected<Bytes, CommonError> onResume() override;
    private:
        bool sslRecv(bool notify);
    private:
        bool m_ready;
        size_t m_length;
        char* m_result_str;
    };

    class SslSendEvent: public SslEvent<std::expected<Bytes, CommonError>> {
    public:
        void reset(SSL* ssl, EventScheduler* scheduler, Bytes&& bytes) {
            m_ssl = ssl;
            m_scheduler = scheduler;
            m_bytes = std::move(bytes);
            m_ready = false;
        }

        std::string name() override { return "SslSendEvent"; }
        void handleEvent() override;
        EventType getEventType() const override { return kEventTypeWrite; }
        bool onReady() override;
        std::expected<Bytes, CommonError> onResume() override;
    private:
        bool sslSend(bool notify);
    private:
        bool m_ready;
        Bytes m_bytes;
    };



}


#endif