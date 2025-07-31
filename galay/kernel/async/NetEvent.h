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
        static AsyncTcpSocketBuilder create(GHandle handle);
        //throw exception
        AsyncTcpSocket build();
        bool check() const;
    private:
        GHandle m_handle{-1};
    };
}

namespace galay::details
{
    struct NetStatusContext
    {
        GHandle m_handle;
        bool m_is_connected = false;
        EventScheduler* m_scheduler = nullptr;
    };

    template <CoType T>
    class NetEvent: public AsyncEvent<T>
    {
    public:
        NetEvent(NetStatusContext& ctx): m_context(ctx) {}
        bool suspend(Waker waker) override {
            this->m_waker = waker;
            return true;
        }

        GHandle getHandle() override { return m_context.m_handle; }
        bool setEventScheduler(EventScheduler* scheduler) override { m_context.m_scheduler = scheduler;  return true; }
        EventScheduler* belongEventScheduler() override { return m_context.m_scheduler; }
    protected:
        NetStatusContext& m_context;
    };

    

    class TcpAcceptEvent: public NetEvent<ValueWrapper<AsyncTcpSocketBuilder>>
    {
    public:
        TcpAcceptEvent(NetStatusContext& ctx) : NetEvent<ValueWrapper<AsyncTcpSocketBuilder>>(ctx) {}
        std::string name() override { return "TcpAcceptEvent"; }
        void handleEvent() override;
        EventType getEventType() override { return EventType::kEventTypeRead; }

        bool ready() override;
        bool suspend(Waker waker) override;
    private:
        bool acceptSocket();
    };

    class TcpRecvEvent: public NetEvent<ValueWrapper<Bytes>>
    {
    public:
        TcpRecvEvent(NetStatusContext& ctx, size_t length);
        std::string name() override { return "TcpRecvEvent"; }
        void handleEvent() override;
        EventType getEventType() override { return EventType::kEventTypeRead; }

        bool ready() override;
        bool suspend(Waker waker) override;
    private:
        bool recvBytes();
    private:
        size_t m_length = 0;
    };

    class TcpSendEvent: public NetEvent<ValueWrapper<Bytes>>
    {
    public:
        TcpSendEvent(NetStatusContext& ctx, Bytes&& bytes);
        std::string name() override { return "TcpSendEvent"; }
        void handleEvent() override;
        EventType getEventType() override { return EventType::kEventTypeWrite; }

        bool ready() override;
        bool suspend(Waker waker) override;
    private:
        bool sendBytes();
    private:
        Bytes m_bytes;
    };

    #ifdef __linux__
    class TcpSendfileEvent: public NetEvent<ValueWrapper<bool>>
    {
    public:
        TcpSendfileEvent(NetStatusContext& ctx, GHandle file_handle, long offset, size_t length) 
            : NetEvent<ValueWrapper<bool>>(ctx), m_file_handle(file_handle), m_offset(offset), m_length(length) {}
        std::string name() override { return "TcpSendfileEvent"; }
        void handleEvent() override;
        EventType getEventType() override { return EventType::kEventTypeWrite; }

        bool ready() override;
        bool suspend(Waker waker) override;
    private:
        GHandle m_file_handle;
        long m_offset;
        size_t m_length;
    };
    #endif

    class TcpConnectEvent: public NetEvent<ValueWrapper<bool>>
    { 
    public:
        TcpConnectEvent(NetStatusContext& ctx, const Host& host);
        std::string name() override { return "TcpConnectEvent"; }
        void handleEvent() override;
        EventType getEventType() override { return EventType::kEventTypeWrite; }

        bool ready() override;
        bool suspend(Waker waker) override;
    private:
        bool connectToHost();
    private:
        Host m_host;
    };

    class TcpCloseEvent: public NetEvent<ValueWrapper<bool>>
    { 
    public:
        TcpCloseEvent(NetStatusContext& ctx);
        std::string name() override { return "TcpCloseEvent"; }
        void handleEvent() override {}
        EventType getEventType() override { return EventType::kEventTypeNone; }

        bool ready() override;
        bool suspend(Waker waker) override;
    };


    class UdpRecvfromEvent: public NetEvent<ValueWrapper<Bytes>>
    {
    public:
        UdpRecvfromEvent(NetStatusContext& ctx, Host& remote, size_t length) : NetEvent<ValueWrapper<Bytes>>(ctx), m_remote(remote), m_length(length) {}
        std::string name() override { return "UdpRecvfromEvent"; }
        void handleEvent() override;
        EventType getEventType() override { return EventType::kEventTypeRead; }

        bool ready() override;
        bool suspend(Waker waker) override;
    private:
        bool recvfromBytes();
    private:
        Host& m_remote;
        size_t m_length;
    };

    class UdpSendtoEvent: public NetEvent<ValueWrapper<Bytes>>
    {
    public:
        UdpSendtoEvent(NetStatusContext& ctx, const Host& remote, Bytes&& bytes) : NetEvent<ValueWrapper<Bytes>>(ctx),  m_remote(remote), m_bytes(std::move(bytes)) {}
        std::string name() override { return "UdpSendtoEvent"; }
        void handleEvent() override;
        EventType getEventType() override { return EventType::kEventTypeWrite; }

        bool ready() override;
        bool suspend(Waker waker) override;
    private:
        bool sendtoBytes();
    private:
        const Host& m_remote;
        Bytes m_bytes;
    };

    class UdpCloseEvent: public NetEvent<ValueWrapper<bool>>
    { 
    public:
        UdpCloseEvent(NetStatusContext& ctx);
        std::string name() override { return "UdpCloseEvent"; }
        void handleEvent() override {}
        EventType getEventType() override { return EventType::kEventTypeNone; }

        bool ready() override;
        bool suspend(Waker waker) override;
    };
}

#endif