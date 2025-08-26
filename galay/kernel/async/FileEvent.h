#ifndef GALAY_FILE_EVENT_H
#define GALAY_FILE_EVENT_H 

#include "galay/kernel/coroutine/Result.hpp"
#include "galay/common/Common.h"
#include "galay/kernel/coroutine/Coroutine.hpp"
#include "Bytes.hpp"
#include <vector>
#include <libaio.h>

namespace galay::details { 

    template<CoType T>
    class FileEvent: public AsyncEvent<T>
    { 
    public:
        FileEvent(GHandle ehandle, EventScheduler* scheduler) 
            :m_ehandle(ehandle), m_scheduler(scheduler) {}
        bool suspend(Waker waker) override {
            using namespace error;
            this->m_waker = waker;
            if(!m_scheduler->activeEvent(this, nullptr)) {
                Error::ptr error = std::make_shared<SystemError>(ErrorCode::CallActiveEventError, errno);
                makeValue(this->m_result, false, error);
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

    class FileCloseEvent: public FileEvent<ValueWrapper<bool>> 
    {
    public:
        FileCloseEvent(GHandle event_handle, EventScheduler* scheduler, GHandle handle);
        std::string name() override { return "FileCloseEvent"; }
        void handleEvent() override {}
        EventType getEventType() const override { return EventType::kEventTypeNone; }
        bool ready() override;
    private:
        GHandle m_handle;
    };
    
    class FileCommitEvent: public FileEvent<ValueWrapper<bool>>
    {
    public:
        FileCommitEvent(GHandle event_handle, EventScheduler* scheduler, io_context_t context, std::vector<iocb>&& iocbs);
        std::string name() override { return "FileCommitEvent"; }
        EventType getEventType() const override { return EventType::kEventTypeRead; }
        bool ready() override;
        void handleEvent() override;
    private:
        size_t m_unfinished_cb;
        io_context_t m_context;
        std::vector<iocb> m_iocbs;
    };

}

#endif