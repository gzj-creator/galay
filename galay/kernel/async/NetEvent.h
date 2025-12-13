#ifndef GALAY_NET_EVENT_H
#define GALAY_NET_EVENT_H

#include "galay/kernel/event/Event.h"
#include "galay/kernel/coroutine/AsyncEvent.hpp"
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

#if defined(USE_IOURING)
    // io_uring Proactor 模式的 NetEvent 基类
    template <CoType T>
    class NetEvent: public AsyncEvent<T>, public Event, public IOResultHolder
    {
    public:
        void handleEvent() override { this->m_waker.wakeUp(); }
        GHandle getHandle() override { return m_handle; }

        // IOResultHolder 接口实现
        void setIOResult(int result) override { m_io_result = result; }

    protected:
        bool m_ready = false;
        GHandle m_handle;
        EventScheduler* m_scheduler;
        int m_io_result = 0;  // io_uring 完成结果
    };
#else
    // epoll/kqueue Reactor 模式的 NetEvent 基类
    template <CoType T>
    class NetEvent: public AsyncEvent<T>, public Event
    {
    public:
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
#endif

    

    class AcceptEvent: public NetEvent<std::expected<void, CommonError>>
    {
    public:
        void reset(GHandle handle, EventScheduler* scheduler, GHandle* accept_handle) {
            m_handle = handle;
            m_scheduler = scheduler;
            m_accept_handle = accept_handle;
            m_ready = false;
#if defined(USE_IOURING)
            m_io_result = 0;
            m_addr_len = sizeof(m_addr);
#endif
        }

        std::string name() override { return "AcceptEvent"; }
        EventType getEventType() const override { return EventType::kEventTypeRead; }
        bool onReady() override;
        std::expected<void, CommonError> onResume() override;
#if defined(USE_IOURING)
        bool onSuspend(Waker waker) override;
#endif
    private:
#if !defined(USE_IOURING)
        bool acceptSocket(bool notify);
#endif
    private:
        GHandle* m_accept_handle = nullptr;
#if defined(USE_IOURING)
        sockaddr m_addr{};
        socklen_t m_addr_len = sizeof(m_addr);
#endif
    };

    class RecvEvent: public NetEvent<std::expected<Bytes, CommonError>>
    {
    public:
        void reset(GHandle handle, EventScheduler* scheduler, char* result, size_t length) {
            m_handle = handle;
            m_scheduler = scheduler;
            m_result_str = result;
            m_length = length;
            m_ready = false;
#if defined(USE_IOURING)
            m_io_result = 0;
#endif
        }

        std::string name() override { return "RecvEvent"; }
        EventType getEventType() const override { return EventType::kEventTypeRead; }
        bool onReady() override;
        std::expected<Bytes, CommonError> onResume() override;
#if defined(USE_IOURING)
        bool onSuspend(Waker waker) override;
#endif
    private:
#if !defined(USE_IOURING)
        bool recvBytes(bool notify);
#endif
    private:
        size_t m_length = 0;
        char* m_result_str = nullptr;
    };

    class SendEvent: public NetEvent<std::expected<Bytes, CommonError>>
    {
    public:
        void reset(GHandle handle, EventScheduler* scheduler, Bytes&& bytes) {
            m_handle = handle;
            m_scheduler = scheduler;
            m_bytes = std::move(bytes);
            m_ready = false;
#if defined(USE_IOURING)
            m_io_result = 0;
#endif
        }

        std::string name() override { return "SendEvent"; }
        EventType getEventType() const override { return EventType::kEventTypeWrite; }
        bool onReady() override;
        std::expected<Bytes, CommonError> onResume() override;
#if defined(USE_IOURING)
        bool onSuspend(Waker waker) override;
#endif
    private:
#if !defined(USE_IOURING)
        bool sendBytes(bool notify);
#endif
    private:
        Bytes m_bytes;
    };

#ifdef __linux__
    class SendfileEvent: public NetEvent<std::expected<long, CommonError>>
    {
    public:
        void reset(GHandle handle, EventScheduler* scheduler, GHandle file_handle, long offset, size_t length) {
            m_handle = handle;
            m_scheduler = scheduler;
            m_file_handle = file_handle;
            m_offset = offset;
            m_length = length;
            m_ready = false;
        }

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
        void reset(GHandle handle, EventScheduler* scheduler, const Host& host) {
            m_handle = handle;
            m_scheduler = scheduler;
            m_host = host;
            m_ready = false;
#if defined(USE_IOURING)
            m_io_result = 0;
#endif
        }

        std::string name() override { return "ConnectEvent"; }
        EventType getEventType() const override { return EventType::kEventTypeWrite; }
        bool onReady() override;
        std::expected<void, CommonError> onResume() override;
#if defined(USE_IOURING)
        bool onSuspend(Waker waker) override;
#endif
    private:
#if !defined(USE_IOURING)
        bool connectToHost(bool notify);
#endif
    private:
        Host m_host;
#if defined(USE_IOURING)
        sockaddr_in m_addr{};
#endif
    };

    class CloseEvent: public NetEvent<std::expected<void, CommonError>>
    {
    public:
        void reset(GHandle handle, EventScheduler* scheduler) {
            m_handle = handle;
            m_scheduler = scheduler;
            m_ready = false;
#if defined(USE_IOURING)
            m_io_result = 0;
#endif
        }

        std::string name() override { return "CloseEvent"; }
#if !defined(USE_IOURING)
        void handleEvent() override {}
#endif
        EventType getEventType() const override { return EventType::kEventTypeNone; }

        bool onReady() override;
#if defined(USE_IOURING)
        bool onSuspend(Waker waker) override;
        std::expected<void, CommonError> onResume() override;
#endif
    };


    class RecvfromEvent: public NetEvent<std::expected<Bytes, CommonError>>
    {
    public:
        void reset(GHandle handle, EventScheduler* scheduler, Host* remote, char* buffer, size_t length) {
            m_handle = handle;
            m_scheduler = scheduler;
            m_remote = remote;
            m_length = length;
            m_buffer = buffer;
            m_ready = false;
#if defined(USE_IOURING)
            m_io_result = 0;
            m_addr_len = sizeof(m_addr);
#endif
        }

        std::string name() override { return "RecvfromEvent"; }
        EventType getEventType() const override { return EventType::kEventTypeRead; }
        bool onReady() override;
        std::expected<Bytes, CommonError> onResume() override;
#if defined(USE_IOURING)
        bool onSuspend(Waker waker) override;
#endif

    private:
#if !defined(USE_IOURING)
        bool recvfromBytes(bool notify);
#endif
    private:
        Host* m_remote = nullptr;
        size_t m_length;
        char* m_buffer;
#if defined(USE_IOURING)
        sockaddr m_addr{};
        socklen_t m_addr_len = sizeof(m_addr);
#endif
    };

    class SendtoEvent: public NetEvent<std::expected<Bytes, CommonError>>
    {
    public:
        void reset(GHandle handle, EventScheduler* scheduler, const Host& remote, Bytes&& bytes) {
            m_handle = handle;
            m_scheduler = scheduler;
            m_remote = remote;
            m_bytes = std::move(bytes);
            m_ready = false;
#if defined(USE_IOURING)
            m_io_result = 0;
#endif
        }

        std::string name() override { return "SendtoEvent"; }
        EventType getEventType() const override { return EventType::kEventTypeWrite; }

        bool onReady() override;
        std::expected<Bytes, CommonError> onResume() override;
#if defined(USE_IOURING)
        bool onSuspend(Waker waker) override;
#endif
    private:
#if !defined(USE_IOURING)
        bool sendtoBytes(bool notify);
#endif
    private:
        Host m_remote;
        Bytes m_bytes;
#if defined(USE_IOURING)
        sockaddr_in m_addr{};
#endif
    };
}

#endif