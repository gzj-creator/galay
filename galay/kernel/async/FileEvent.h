#ifndef GALAY_FILE_EVENT_H
#define GALAY_FILE_EVENT_H 

#include "galay/kernel/coroutine/Result.hpp"
#include "galay/common/Common.h"
#include "galay/kernel/coroutine/Coroutine.hpp"
#include "Bytes.hpp"
#include <vector>
#include <libaio.h>

namespace galay::details { 


    struct FileStatusContext
    {
        GHandle m_handle;
        GHandle m_event_handle;
        io_context_t m_io_ctx;
        int32_t m_iocb_index = 0;
        std::vector<iocb> m_iocbs;
        int32_t m_unfinished = 0;
        EventScheduler* m_scheduler = nullptr;
    };

    template<CoType T>
    class FileEvent: public AsyncEvent<T>
    { 
    public:
        FileEvent(FileStatusContext& context) :m_context(context) {}
        bool suspend(Waker waker) override {
            this->m_waker = waker;
            return true;
        }
        GHandle& getHandle() override {  return m_context.m_event_handle; }
    protected:
        FileStatusContext& m_context;
    };

    class FileCloseEvent: public FileEvent<ValueWrapper<bool>> 
    {
    public:
        FileCloseEvent(FileStatusContext& context);
        std::string name() override { return "FileCloseEvent"; }
        void handleEvent() override;
        EventType getEventType() const override { return EventType::kEventTypeNone; }

        bool ready() override;
        bool suspend(Waker waker) override;
    };
    
    class FileCommitEvent: public FileEvent<ValueWrapper<bool>>
    {
    public:
        FileCommitEvent(FileStatusContext& context);
        std::string name() override { return "FileCommitEvent"; }
        void handleEvent() override;
        EventType getEventType() const override { return EventType::kEventTypeRead; }

        bool ready() override;
        bool suspend(Waker waker) override;
    };

}

#endif