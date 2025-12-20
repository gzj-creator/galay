#ifndef GALAY_FILE_EVENT_H
#define GALAY_FILE_EVENT_H

#include "galay/common/Common.h"
#include "galay/kernel/coroutine/Coroutine.hpp"
#include "galay/kernel/event/Event.h"
#include "galay/kernel/coroutine/AsyncEvent.hpp"
#include "Bytes.h"

#ifdef USE_AIO
#include <libaio.h>
#endif

namespace galay::details {

#if defined(USE_IOURING)
    // io_uring Proactor 模式的 FileEvent 基类
    template<CoType T>
    class FileEvent: public AsyncEvent<T>, public Event, public IOResultHolder
    {
    public:
        // 默认的 onSuspend 实现，子类必须覆盖此方法
        bool onSuspend(Waker waker) override {
            this->m_waker = waker;
            // 子类应该覆盖此方法并提交实际的 io_uring 操作
            return true;
        }

        void handleEvent() override { this->m_waker.wakeUp(); }
        GHandle getHandle() override { return m_handle; }

        // IOResultHolder 接口实现
        void setIOResult(int result) override { m_io_result = result; }

    protected:
        bool m_ready = false;
        GHandle m_handle = GHandle::invalid();
        EventScheduler* m_scheduler = nullptr;
        int m_io_result = 0;
    };

    class FileCloseEvent: public FileEvent<std::expected<void, CommonError>>
    {
    public:
        void reset(GHandle handle, EventScheduler* scheduler) {
            m_handle = handle;
            m_scheduler = scheduler;
            m_io_result = 0;
        }
        std::string name() override { return "FileCloseEvent"; }
        EventType getEventType() const override { return EventType::kEventTypeNone; }
        bool onReady() override;
        bool onSuspend(Waker waker) override;
        std::expected<void, CommonError> onResume() override;
    };

    class FileReadEvent: public FileEvent<std::expected<Bytes, CommonError>>
    {
    public:
        void reset(GHandle handle, EventScheduler* scheduler, char* buffer, size_t length) {
            m_handle = handle;
            m_scheduler = scheduler;
            m_buffer = buffer;
            m_length = length;
            m_io_result = 0;
        }
        std::string name() override { return "FileReadEvent"; }
        EventType getEventType() const override { return EventType::kEventTypeRead; }
        bool onReady() override;
        bool onSuspend(Waker waker) override;
        std::expected<Bytes, CommonError> onResume() override;
    private:
        size_t m_length = 0;
        char* m_buffer = nullptr;
    };

    class FileWriteEvent: public FileEvent<std::expected<Bytes, CommonError>>
    {
    public:
        void reset(GHandle handle, EventScheduler* scheduler, Bytes&& bytes) {
            m_handle = handle;
            m_scheduler = scheduler;
            m_bytes = std::move(bytes);
            m_io_result = 0;
        }
        std::string name() override { return "FileWriteEvent"; }
        EventType getEventType() const override { return EventType::kEventTypeWrite; }
        bool onReady() override;
        bool onSuspend(Waker waker) override;
        std::expected<Bytes, CommonError> onResume() override;
    private:
        Bytes m_bytes = Bytes();
    };

#elif defined(USE_AIO)
    template<CoType T>
    class FileEvent: public AsyncEvent<T>, public Event
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
        GHandle getHandle() override {  return m_ehandle; }
    protected:
        GHandle m_ehandle;
        EventScheduler* m_scheduler;
    };

    class FileCloseEvent: public FileEvent<std::expected<void, CommonError>>
    {
    public:
        void reset(GHandle event_handle, EventScheduler* scheduler, GHandle handle) {
            m_ehandle = event_handle;
            m_scheduler = scheduler;
            m_handle = handle;
        }
        std::string name() override { return "FileCloseEvent"; }
        void handleEvent() override {}
        EventType getEventType() const override { return EventType::kEventTypeNone; }
        bool onReady() override;
    private:
        GHandle m_handle;
    };

    class AioGetEvent: public FileEvent<std::expected<std::vector<io_event>, CommonError>>
    {
    public:
        void reset(GHandle event_handle, EventScheduler* scheduler, io_context_t context, uint64_t* expect_events) {
            m_ehandle = event_handle;
            m_scheduler = scheduler;
            m_context = context;
            m_expect_events = expect_events;
        }
        std::string name() override { return "AioGetEvent"; }
        EventType getEventType() const override { return EventType::kEventTypeRead; }
        bool onReady() override;
        void handleEvent() override;
        std::expected<std::vector<io_event>, CommonError> onResume() override;
    private:
        bool getEvent(bool notify);
    private:
        bool m_ready = false;
        io_context_t m_context;
        uint64_t* m_expect_events = nullptr;
    };

#else
    // epoll/kqueue Reactor 模式的 FileEvent 基类
    template<CoType T>
    class FileEvent: public AsyncEvent<T>, public Event
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
        GHandle getHandle() override {  return m_handle; }
    protected:
        bool m_ready = false;
        GHandle m_handle = GHandle::invalid();
        EventScheduler* m_scheduler = nullptr;
    };

    class FileCloseEvent: public FileEvent<std::expected<void, CommonError>> 
    {
    public:
        void reset(GHandle handle, EventScheduler* scheduler) {
            m_handle = handle;
            m_scheduler = scheduler;
        }
        std::string name() override { return "FileCloseEvent"; }
        void handleEvent() override {}
        EventType getEventType() const override { return EventType::kEventTypeNone; }
        bool onReady() override;
    };

    class FileReadEvent: public FileEvent<std::expected<Bytes, CommonError>>
    {
    public:
        void reset(GHandle handle, EventScheduler* scheduler, char* buffer, size_t length) {
            m_handle = handle;
            m_scheduler = scheduler;
            m_buffer = buffer;
            m_length = length;
        }
        std::string name() override { return "FileReadEvent"; }
        EventType getEventType() const override { return EventType::kEventTypeRead; }
        bool onReady() override;
        std::expected<Bytes, CommonError> onResume() override;
    private:
        bool readBytes(bool notify);
    private:
        size_t m_length = 0;
        char* m_buffer = nullptr;
    };

    class FileWriteEvent: public FileEvent<std::expected<Bytes, CommonError>>
    {
    public:
        void reset(GHandle handle, EventScheduler* scheduler, Bytes&& bytes) {
            m_handle = handle;
            m_scheduler = scheduler;
            m_bytes = std::move(bytes);
        }
        std::string name() override { return "FileWriteEvent"; }
        EventType getEventType() const override { return EventType::kEventTypeWrite; }
        bool onReady() override;
        std::expected<Bytes, CommonError> onResume() override;
    private:
        bool writeBytes(bool notify);
    private:
        Bytes m_bytes = Bytes();
    };
#endif
}

#endif