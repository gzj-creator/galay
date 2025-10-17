#ifndef GALAY_NET_EVENT_H
#define GALAY_NET_EVENT_H

#include "galay/kernel/coroutine/Result.hpp"
#include "galay/common/Common.h"
#include "Bytes.h"

namespace galay
{
    class AsyncTcpSocket;
    class AsyncTcpSocketBuilder {
        friend class AsyncTcpSocket;
    public:
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
        bool onSuspend(Waker waker) override {
            using namespace error;
            this->m_waker = waker;
            if(!m_scheduler->activeEvent(this, nullptr)) {
                this->m_result = std::unexpected(CommonError(CallActiveEventError, static_cast<uint32_t>(errno)));
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

    

    class AcceptEvent: public NetEvent<std::expected<void, CommonError>>
    {
    public:
        AcceptEvent(GHandle handle, EventScheduler* scheduler, GHandle& accept_handle) 
            : NetEvent<std::expected<void, CommonError>>(handle, scheduler), m_accept_handle(accept_handle) {}
        std::string name() override { return "AcceptEvent"; }
        EventType getEventType() const override { return EventType::kEventTypeRead; }
        bool onReady() override;
        std::expected<void, CommonError> onResume() override;  
    private:
        bool acceptSocket(bool notify);
    private:
        GHandle& m_accept_handle;
    };

    class RecvEvent: public NetEvent<std::expected<Bytes, CommonError>>
    {
    public:
        RecvEvent(GHandle handle, EventScheduler* scheduler, char* result, size_t length);
        std::string name() override { return "RecvEvent"; }
        EventType getEventType() const override { return EventType::kEventTypeRead; }
        bool onReady() override;
        std::expected<Bytes, CommonError> onResume() override;
    private:
        bool recvBytes(bool notify);
    private:
        size_t m_length = 0;
        char* m_result_str = 0;
    };

    class SendEvent: public NetEvent<std::expected<Bytes, CommonError>>
    {
    public:
        SendEvent(GHandle handle, EventScheduler* scheduler, Bytes&& bytes);
        std::string name() override { return "SendEvent"; }
        EventType getEventType() const override { return EventType::kEventTypeWrite; }
        bool onReady() override;
        std::expected<Bytes, CommonError> onResume() override;
    private:
        bool sendBytes(bool notify);
    private:
        Bytes m_bytes;
    };

#ifdef __linux__
    class SendfileEvent: public NetEvent<std::expected<long, CommonError>>
    {
    public:
        SendfileEvent(GHandle handle, EventScheduler* scheduler, GHandle file_handle, long offset, size_t length) 
            : NetEvent<std::expected<long, CommonError>>(handle, scheduler), m_file_handle(file_handle), m_offset(offset), m_length(length) {}
        std::string name() override { return "SendfileEvent"; }
        EventType getEventType() const override { return EventType::kEventTypeWrite; }
        bool onReady() override;
        std::expected<long, CommonError> onResume() override;
    private:
        bool sendfile(bool notify);
    private:
        GHandle m_file_handle;
        long m_offset;
        size_t m_length;
    };
#endif

    class ConnectEvent: public NetEvent<std::expected<void, CommonError>>
    { 
    public:
        ConnectEvent(GHandle handle, EventScheduler* scheduler, const Host& host);
        std::string name() override { return "ConnectEvent"; }
        EventType getEventType() const override { return EventType::kEventTypeWrite; }
        bool onReady() override;
        std::expected<void, CommonError> onResume() override;
    private:
        bool connectToHost(bool notify);
    private:
        Host m_host;
    };

    class CloseEvent: public NetEvent<std::expected<void, CommonError>>
    { 
    public:
        CloseEvent(GHandle handle, EventScheduler* scheduler);
        std::string name() override { return "CloseEvent"; }
        void handleEvent() override {}
        EventType getEventType() const override { return EventType::kEventTypeNone; }

        bool onReady() override;
    };


    class RecvfromEvent: public NetEvent<std::expected<Bytes, CommonError>>
    {
    public:
        RecvfromEvent(GHandle handle, EventScheduler* scheduler, Host& remote, char* buffer, size_t length) 
            : NetEvent<std::expected<Bytes, CommonError>>(handle, scheduler), m_remote(remote), m_length(length), m_buffer(buffer) {}
        std::string name() override { return "RecvfromEvent"; }
        EventType getEventType() const override { return EventType::kEventTypeRead; }
        bool onReady() override;
        std::expected<Bytes, CommonError> onResume() override;

    private:
        bool recvfromBytes(bool notify);
    private:
        Host& m_remote;
        size_t m_length;
        char* m_buffer;
    };

    class SendtoEvent: public NetEvent<std::expected<Bytes, CommonError>>
    {
    public:
        SendtoEvent(GHandle handle, EventScheduler* scheduler, const Host& remote, Bytes&& bytes) 
            : NetEvent<std::expected<Bytes, CommonError>>(handle, scheduler),  m_remote(remote), m_bytes(std::move(bytes)) {}
        std::string name() override { return "SendtoEvent"; }
        EventType getEventType() const override { return EventType::kEventTypeWrite; }

        bool onReady() override;
        std::expected<Bytes, CommonError> onResume() override;  
    private:
        bool sendtoBytes(bool notify);
    private:
        const Host& m_remote;
        Bytes m_bytes;
    };
}

#endif