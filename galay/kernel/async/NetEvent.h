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
    class NetEvent: public AsyncEvent<T>, public Event
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

    

    class AcceptEvent: public NetEvent<ValueWrapper<AsyncTcpSocketBuilder>>
    {
    public:
        AcceptEvent(GHandle handle, EventScheduler* scheduler) 
            : NetEvent<ValueWrapper<AsyncTcpSocketBuilder>>(handle, scheduler) {}
        std::string name() override { return "AcceptEvent"; }
        EventType getEventType() const override { return EventType::kEventTypeRead; }
        bool ready() override;
        ValueWrapper<AsyncTcpSocketBuilder> resume() override;  
    private:
        bool acceptSocket(bool notify);
    };

    class RecvEvent: public NetEvent<ValueWrapper<Bytes>>
    {
    public:
        RecvEvent(GHandle handle, EventScheduler* scheduler, char* buffer, size_t length);
        std::string name() override { return "RecvEvent"; }
        EventType getEventType() const override { return EventType::kEventTypeRead; }
        bool ready() override;
        ValueWrapper<Bytes> resume() override;
    private:
        bool recvBytes(bool notify);
    private:
        size_t m_length = 0;
        char* m_buffer = nullptr;
    };

    class SendEvent: public NetEvent<ValueWrapper<Bytes>>
    {
    public:
        SendEvent(GHandle handle, EventScheduler* scheduler, Bytes&& bytes);
        std::string name() override { return "SendEvent"; }
        EventType getEventType() const override { return EventType::kEventTypeWrite; }
        bool ready() override;
        ValueWrapper<Bytes> resume() override;
    private:
        bool sendBytes(bool notify);
    private:
        Bytes m_bytes;
    };

#ifdef __linux__
    class SendfileEvent: public NetEvent<ValueWrapper<long>>
    {
    public:
        SendfileEvent(GHandle handle, EventScheduler* scheduler, GHandle file_handle, long offset, size_t length) 
            : NetEvent<ValueWrapper<long>>(handle, scheduler), m_file_handle(file_handle), m_offset(offset), m_length(length) {}
        std::string name() override { return "SendfileEvent"; }
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

    class ConnectEvent: public NetEvent<ValueWrapper<bool>>
    { 
    public:
        ConnectEvent(GHandle handle, EventScheduler* scheduler, const Host& host);
        std::string name() override { return "ConnectEvent"; }
        EventType getEventType() const override { return EventType::kEventTypeWrite; }
        bool ready() override;
        ValueWrapper<bool> resume() override;
    private:
        bool connectToHost(bool notify);
    private:
        Host m_host;
    };

    class CloseEvent: public NetEvent<ValueWrapper<bool>>
    { 
    public:
        CloseEvent(GHandle handle, EventScheduler* scheduler);
        std::string name() override { return "CloseEvent"; }
        void handleEvent() override {}
        EventType getEventType() const override { return EventType::kEventTypeNone; }

        bool ready() override;
    };


    class RecvfromEvent: public NetEvent<ValueWrapper<Bytes>>
    {
    public:
        RecvfromEvent(GHandle handle, EventScheduler* scheduler, Host& remote, char* buffer, size_t length) 
            : NetEvent<ValueWrapper<Bytes>>(handle, scheduler), m_remote(remote), m_length(length), m_buffer(buffer) {}
        std::string name() override { return "RecvfromEvent"; }
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

    class SendtoEvent: public NetEvent<ValueWrapper<Bytes>>
    {
    public:
        SendtoEvent(GHandle handle, EventScheduler* scheduler, const Host& remote, Bytes&& bytes) 
            : NetEvent<ValueWrapper<Bytes>>(handle, scheduler),  m_remote(remote), m_bytes(std::move(bytes)) {}
        std::string name() override { return "SendtoEvent"; }
        EventType getEventType() const override { return EventType::kEventTypeWrite; }

        bool ready() override;
        ValueWrapper<Bytes> resume() override;  
    private:
        bool sendtoBytes(bool notify);
    private:
        const Host& m_remote;
        Bytes m_bytes;
    };
}

#endif