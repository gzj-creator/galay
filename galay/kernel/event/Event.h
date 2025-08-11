#ifndef GALAY_EVENT_H
#define GALAY_EVENT_H 

#include "EventScheduler.h"

namespace galay::details 
{ 

extern std::string toString(EventType type);

//must be alloc at heap
class Event
{
public:
    virtual std::string name() = 0;
    virtual void handleEvent() = 0;
    virtual EventType getEventType() const = 0;
    virtual GHandle& getHandle() = 0;
    virtual ~Event() = default;
    bool cancel();
private:
    std::atomic_bool m_cancel = false;
};

class CallbackEvent final : public Event
{
public:
    class EventDeletor {
    public:
        EventDeletor(Event* event): m_event(event) {}
        EventDeletor(const EventDeletor&) = delete;
        EventDeletor& operator=(const EventDeletor&) = delete;
        EventDeletor(EventDeletor&& other) {
            m_event = other.m_event;
            other.m_event = nullptr;
        }
        ~EventDeletor() { if(m_event) delete m_event; }
    private:
        Event* m_event;
    };

    CallbackEvent(GHandle handle, EventType type, std::function<void(Event*, EventDeletor)> callback);
    void handleEvent() override;
    std::string name() override { return "CallbackEvent"; }
    EventType getEventType() const override { return m_type; }
    GHandle& getHandle() override { return m_handle; }
    ~CallbackEvent() override;
private:
    EventType m_type;
    GHandle m_handle;
    std::atomic<EventScheduler*> m_scheduler;
    std::function<void(Event*, EventDeletor)> m_callback;
};


}


#endif  