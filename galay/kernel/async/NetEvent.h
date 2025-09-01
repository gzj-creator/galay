#ifndef GALAY_NET_EVENT_H
#define GALAY_NET_EVENT_H

#include "galay/kernel/coroutine/Result.hpp"
#include "galay/common/Common.h"
#include "Bytes.h"

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
        void handleEvent() override { this->m_waker.wakeUp(); }
        GHandle getHandle() override { return m_handle; }

    protected:
        bool m_ready = false;
        GHandle m_handle;
        EventScheduler* m_scheduler;
    };

    

    class TcpAcceptEvent: public NetEvent<ValueWrapper<AsyncTcpSocketBuilder>>
    {
    public:
        TcpAcceptEvent(GHandle handle, EventScheduler* scheduler) 
            : NetEvent<ValueWrapper<AsyncTcpSocketBuilder>>(handle, scheduler) {}
        std::string name() override { return "TcpAcceptEvent"; }
        EventType getEventType() const override { return EventType::kEventTypeRead; }
        bool ready() override;
        ValueWrapper<AsyncTcpSocketBuilder> resume() override;  
    private:
        bool acceptSocket(bool notify);
    };

    class TcpRecvEvent: public NetEvent<ValueWrapper<Bytes>>
    {
    public:
        TcpRecvEvent(GHandle handle, EventScheduler* scheduler, char* buffer, size_t length);
        std::string name() override { return "TcpRecvEvent"; }
        EventType getEventType() const override { return EventType::kEventTypeRead; }
        bool ready() override;
        ValueWrapper<Bytes> resume() override;
    private:
        bool recvBytes(bool notify);
    private:
        size_t m_length = 0;
        char* m_buffer = nullptr;
    };

    class TcpSendEvent: public NetEvent<ValueWrapper<Bytes>>
    {
    public:
        TcpSendEvent(GHandle handle, EventScheduler* scheduler, Bytes&& bytes);
        std::string name() override { return "TcpSendEvent"; }
        EventType getEventType() const override { return EventType::kEventTypeWrite; }
        bool ready() override;
        ValueWrapper<Bytes> resume() override;
    private:
        bool sendBytes(bool notify);
    private:
        Bytes m_bytes;
    };

#ifdef __linux__
    class TcpSendfileEvent: public NetEvent<ValueWrapper<long>>
    {
    public:
        TcpSendfileEvent(GHandle handle, EventScheduler* scheduler, GHandle file_handle, long offset, size_t length) 
            : NetEvent<ValueWrapper<long>>(handle, scheduler), m_file_handle(file_handle), m_offset(offset), m_length(length) {}
        std::string name() override { return "TcpSendfileEvent"; }
        EventType getEventType() const override { return EventType::kEventTypeWrite; }
        bool ready() override;
        ValueWrapper<long> resume() override;
    private:
        bool sendfile(bool notify);
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
        EventType getEventType() const override { return EventType::kEventTypeWrite; }
        bool ready() override;
        ValueWrapper<bool> resume() override;
    private:
        bool connectToHost(bool notify);
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
        UdpRecvfromEvent(GHandle handle, EventScheduler* scheduler, Host& remote, char* buffer, size_t length) 
            : NetEvent<ValueWrapper<Bytes>>(handle, scheduler), m_remote(remote), m_length(length), m_buffer(buffer) {}
        std::string name() override { return "UdpRecvfromEvent"; }
        EventType getEventType() const override { return EventType::kEventTypeRead; }
        bool ready() override;
        ValueWrapper<Bytes> resume() override;

    private:
        bool recvfromBytes(bool notify);
    private:
        Host& m_remote;
        size_t m_length;
        char* m_buffer;
    };

    class UdpSendtoEvent: public NetEvent<ValueWrapper<Bytes>>
    {
    public:
        UdpSendtoEvent(GHandle handle, EventScheduler* scheduler, const Host& remote, Bytes&& bytes) 
            : NetEvent<ValueWrapper<Bytes>>(handle, scheduler),  m_remote(remote), m_bytes(std::move(bytes)) {}
        std::string name() override { return "UdpSendtoEvent"; }
        EventType getEventType() const override { return EventType::kEventTypeWrite; }

        bool ready() override;
        ValueWrapper<Bytes> resume() override;  
    private:
        bool sendtoBytes(bool notify);
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