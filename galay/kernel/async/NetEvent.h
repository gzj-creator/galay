#ifndef GALAY_NET_EVENT_H
#define GALAY_NET_EVENT_H

#include "galay/kernel/coroutine/Result.hpp"
#include "galay/common/Log.h"
#include "galay/common/Common.h"
#include "Bytes.hpp"

namespace galay
{
    class AsyncTcpSocket;
    class AsyncUdpSocket;

    class AsyncTcpSocketBuilder {
        friend class AsyncTcpSocket;
    public:
        static AsyncTcpSocketBuilder create(EventScheduler* scheduler, GHandle handle);
        //throw exception
        AsyncTcpSocket build();
        bool check() const;
    private:
        GHandle m_handle;
        EventScheduler* m_scheduler = nullptr;
    };
}

namespace galay::details
{

    template <CoType T>
    class NetEvent: public AsyncEvent<T>
    {
    public:
        NetEvent(GHandle handle, EventScheduler* scheduler)
            : m_handle(handle), m_scheduler(scheduler) {}
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
        GHandle getHandle() override { return m_handle; }

    protected:
        GHandle m_handle;
        EventScheduler* m_scheduler;
    };

    

    class TcpAcceptEvent: public NetEvent<ValueWrapper<AsyncTcpSocketBuilder>>
    {
    public:
        TcpAcceptEvent(GHandle handle, EventScheduler* scheduler) 
            : NetEvent<ValueWrapper<AsyncTcpSocketBuilder>>(handle, scheduler) {}
        std::string name() override { return "TcpAcceptEvent"; }
        void handleEvent() override;
        EventType getEventType() const override { return EventType::kEventTypeRead; }

        bool ready() override;
        
    private:
        bool acceptSocket();
    };

    class TcpRecvEvent: public NetEvent<ValueWrapper<Bytes>>
    {
    public:
        TcpRecvEvent(GHandle handle, EventScheduler* scheduler, size_t length);
        std::string name() override { return "TcpRecvEvent"; }
        void handleEvent() override;
        EventType getEventType() const override { return EventType::kEventTypeRead; }

        bool ready() override;
    private:
        bool recvBytes();
    private:
        size_t m_length = 0;
    };

    class TcpSendEvent: public NetEvent<ValueWrapper<Bytes>>
    {
    public:
        TcpSendEvent(GHandle handle, EventScheduler* scheduler, Bytes&& bytes);
        std::string name() override { return "TcpSendEvent"; }
        void handleEvent() override;
        EventType getEventType() const override { return EventType::kEventTypeWrite; }

        bool ready() override;
        
    private:
        bool sendBytes();
    private:
        Bytes m_bytes;
    };

    #ifdef __linux__
    class TcpSendfileEvent: public NetEvent<ValueWrapper<bool>>
    {
    public:
        TcpSendfileEvent(GHandle handle, EventScheduler* scheduler, GHandle file_handle, long offset, size_t length) 
            : NetEvent<ValueWrapper<bool>>(handle, scheduler), m_file_handle(file_handle), m_offset(offset), m_length(length) {}
        std::string name() override { return "TcpSendfileEvent"; }
        void handleEvent() override;
        EventType getEventType() const override { return EventType::kEventTypeWrite; }

        bool ready() override;
        
    private:
        GHandle m_file_handle;
        long m_offset;
        size_t m_length;
    };
    #endif

    class TcpConnectEvent: public NetEvent<ValueWrapper<bool>>
    { 
    public:
        TcpConnectEvent(GHandle handle, EventScheduler* scheduler, const Host& host);
        std::string name() override { return "TcpConnectEvent"; }
        void handleEvent() override;
        EventType getEventType() const override { return EventType::kEventTypeWrite; }

        bool ready() override;
        
    private:
        bool connectToHost();
    private:
        Host m_host;
    };

    class TcpCloseEvent: public NetEvent<ValueWrapper<bool>>
    { 
    public:
        TcpCloseEvent(GHandle handle, EventScheduler* scheduler);
        std::string name() override { return "TcpCloseEvent"; }
        void handleEvent() override {}
        EventType getEventType() const override { return EventType::kEventTypeNone; }

        bool ready() override;
    };


    class UdpRecvfromEvent: public NetEvent<ValueWrapper<Bytes>>
    {
    public:
        UdpRecvfromEvent(GHandle handle, EventScheduler* scheduler, Host& remote, size_t length) 
            : NetEvent<ValueWrapper<Bytes>>(handle, scheduler), m_remote(remote), m_length(length) {}
        std::string name() override { return "UdpRecvfromEvent"; }
        void handleEvent() override;
        EventType getEventType() const override { return EventType::kEventTypeRead; }

        bool ready() override;
    private:
        bool recvfromBytes();
    private:
        Host& m_remote;
        size_t m_length;
    };

    class UdpSendtoEvent: public NetEvent<ValueWrapper<Bytes>>
    {
    public:
        UdpSendtoEvent(GHandle handle, EventScheduler* scheduler, const Host& remote, Bytes&& bytes) 
            : NetEvent<ValueWrapper<Bytes>>(handle, scheduler),  m_remote(remote), m_bytes(std::move(bytes)) {}
        std::string name() override { return "UdpSendtoEvent"; }
        void handleEvent() override;
        EventType getEventType() const override { return EventType::kEventTypeWrite; }

        bool ready() override;
    private:
        bool sendtoBytes();
    private:
        const Host& m_remote;
        Bytes m_bytes;
    };

    class UdpCloseEvent: public NetEvent<ValueWrapper<bool>>
    { 
    public:
        UdpCloseEvent(GHandle handle, EventScheduler* scheduler);
        std::string name() override { return "UdpCloseEvent"; }
        void handleEvent() override {}
        EventType getEventType() const override { return EventType::kEventTypeNone; }

        bool ready() override;
    };
}

#endif