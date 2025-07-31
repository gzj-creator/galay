#ifndef GALAY_FILE_EVENT_H
#define GALAY_FILE_EVENT_H 

#include "galay/kernel/coroutine/Result.hpp"
#include "galay/common/Common.h"
#include "galay/kernel/coroutine/Coroutine.hpp"


namespace galay::details { 

    template<CoType T>
    class FileEvent: public AsyncEvent<ValueWrapper<T>>
    { 
    public:
        FileEvent(GHandle handle) :m_handle(handle) {}
        bool suspend(Waker waker) override {
            this->m_waker = waker;
            return true;
        }
        GHandle getHandle() override {  return m_handle;    }
        bool setEventScheduler(EventScheduler* scheduler) override { m_scheduler = scheduler;  return true; }
        EventScheduler* belongEventScheduler() override { return m_scheduler; }
        virtual ~FileEvent() {
            if(m_scheduler) m_scheduler->delEvent(this, nullptr);
        }
    protected:
        GHandle m_handle = {};
        EventScheduler* m_scheduler = nullptr;
    };

    class FileOpenEvent: public FileEvent<ValueWrapper<bool>> 
    {
    public:
        FileOpenEvent(const std::string& path);
        
    private:
        
    };
    


}

#endif