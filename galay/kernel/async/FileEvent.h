#ifndef GALAY_FILE_EVENT_H
#define GALAY_FILE_EVENT_H 

#include "galay/kernel/coroutine/Result.hpp"
#include "galay/common/Common.h"
#include "galay/kernel/coroutine/Coroutine.hpp"
#include "Bytes.h"
#include <vector>

#ifdef USE_AIO
#include <libaio.h>
#endif

namespace galay::details { 

#ifdef USE_AIO
    template<CoType T>
    class FileEvent: public AsyncEvent<T>, public Event
    { 
    public:
        FileEvent(GHandle ehandle, EventScheduler* scheduler) 
            :m_ehandle(ehandle), m_scheduler(scheduler) {}
        bool onSuspend(Waker waker) override {
            using namespace error;
            this->m_waker = waker;
            std::cout << "FileEvent::onSuspend" << std::endl;
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
        FileCloseEvent(GHandle event_handle, EventScheduler* scheduler, GHandle handle);
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
        AioGetEvent(GHandle event_handle, EventScheduler* scheduler, io_context_t context, uint64_t& expect_events);
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
        uint64_t& m_expect_events;
    };

#else
    template<CoType T>
    class FileEvent: public AsyncEvent<T>, public Event
    { 
    public:
        FileEvent(GHandle handle, EventScheduler* scheduler) 
            :m_handle(handle), m_scheduler(scheduler) {}
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
        GHandle m_handle;
        EventScheduler* m_scheduler;
    };

    class FileCloseEvent: public FileEvent<std::expected<void, CommonError>> 
    {
    public:
        FileCloseEvent(GHandle handle, EventScheduler* scheduler);
        std::string name() override { return "FileCloseEvent"; }
        void handleEvent() override {}
        EventType getEventType() const override { return EventType::kEventTypeNone; }
        bool onReady() override;
    private:
        GHandle m_handle;
    };

    class FileReadEvent: public FileEvent<std::expected<Bytes, CommonError>>
    {
    public:
        FileReadEvent(GHandle handle, EventScheduler* scheduler, char* buffer, size_t length);
        std::string name() override { return "FileReadEvent"; }
        EventType getEventType() const override { return EventType::kEventTypeRead; }
        bool onReady() override;
        std::expected<Bytes, CommonError> onResume() override;
    private:
        bool readBytes(bool notify);
    private:
        size_t m_length;
        char* m_buffer;
    };

    class FileWriteEvent: public FileEvent<std::expected<Bytes, CommonError>>
    {
    public:
        FileWriteEvent(GHandle handle, EventScheduler* scheduler, Bytes&& bytes);
        std::string name() override { return "FileWriteEvent"; }
        EventType getEventType() const override { return EventType::kEventTypeWrite; }
        bool onReady() override;
        std::expected<Bytes, CommonError> onResume() override;
    private:
        bool writeBytes(bool notify);
    private:
        Bytes m_bytes;
    };
#endif
}

#endif