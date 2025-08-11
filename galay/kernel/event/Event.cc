#include "Event.h"

namespace galay::details { 

    std::string toString(const EventType type)
    {
        switch (type)
        {
        case kEventTypeNone:
            return "EventTypeNone";
        case kEventTypeRead:
            return "EventTypeRead";
        case kEventTypeWrite:
            return "EventTypeWrite";
        case kEventTypeTimer:
            return "EventTypeTimer";
        case kEventTypeError:
            return "EventTypeError";
        default:
            break;
        }
        return ""; 
    }

    bool Event::cancel()
    {
        bool old = false;
        return m_cancel.compare_exchange_strong(old, true);
    }

    CallbackEvent::CallbackEvent(const GHandle handle, const EventType type, std::function<void(Event*, EventDeletor)> callback)
        : m_type(type), m_handle(handle), m_scheduler(nullptr), m_callback(std::move(callback))
    {
        
    }

    void CallbackEvent::handleEvent()
    {
        this->m_callback(this, EventDeletor(this));
    }

    CallbackEvent::~CallbackEvent()
    {
        if( m_scheduler ) m_scheduler.load()->delEvent(this, nullptr);
    }

}