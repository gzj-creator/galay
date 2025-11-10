#include "EventDispatch.h"
#include "Event.h"
#include "common/Log.h"

#if defined(__linux__)
    #include <sys/epoll.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    #include <sys/event.h>
    // 为了统一接口，定义 EPOLLIN/EPOLLOUT/EPOLLERR 常量
    #ifndef EPOLLIN
        #define EPOLLIN  0x01
    #endif
    #ifndef EPOLLOUT
        #define EPOLLOUT 0x02
    #endif
    #ifndef EPOLLERR
        #define EPOLLERR 0x04
    #endif
    #ifndef EPOLLTIMER
        #define EPOLLTIMER 0x08  // 定时器事件标志（仅用于 macOS/BSD）
    #endif
#endif

namespace galay {

void EventDispatcher::addReadEvent(details::Event* event)
{
    read_event = event;
    registered_events.fetch_or(Read, std::memory_order_acq_rel);
}

void EventDispatcher::addWriteEvent(details::Event* event)
{
    write_event = event;
    registered_events.fetch_or(Write, std::memory_order_acq_rel);
}

void EventDispatcher::addErrorEvent(details::Event* event)
{
    error_event = event;
    registered_events.fetch_or(Error, std::memory_order_acq_rel);
}

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
void EventDispatcher::addTimerEvent(details::Event* event)
{
    timer_event = event;
    registered_events.fetch_or(Timer, std::memory_order_acq_rel);
}
#endif

void EventDispatcher::removeReadEvent()
{
    registered_events.fetch_and(~Read, std::memory_order_acq_rel);
    read_event = nullptr;
}

void EventDispatcher::removeWriteEvent()
{
    registered_events.fetch_and(~Write, std::memory_order_acq_rel);
    write_event = nullptr;
}

void EventDispatcher::removeErrorEvent()
{
    registered_events.fetch_and(~Error, std::memory_order_acq_rel);
    error_event = nullptr;
}

#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
void EventDispatcher::removeTimerEvent()
{
    registered_events.fetch_and(~Timer, std::memory_order_acq_rel);
    timer_event = nullptr;
}
#endif

void EventDispatcher::dispatch(uint32_t triggered_events)
{
    // 根据触发的事件类型，移除状态并调用对应的Event
    // 注意：多个事件可能同时触发（例如：同时可读可写）
    // 无论是kqueue还是epoll,都需要移除event指针,因为event对象的生命周期由协程管理
    // 当协程resume后,event可能被销毁,所以必须在handleEvent前移除指针

    const char* read_event_name = read_event ? read_event->name().c_str() : "null";
    const char* write_event_name = write_event ? write_event->name().c_str() : "null";
    LogInfo("dispatch, triggered_events: {}, read_event: {}, write_event: {}",  
            triggered_events, read_event_name, write_event_name);
    
    if ((triggered_events & EPOLLIN) && read_event != nullptr) {
        auto* event = read_event;
        removeReadEvent();
        event->handleEvent();
    }
    
    if ((triggered_events & EPOLLOUT) && write_event != nullptr) {
        auto* event = write_event;
        removeWriteEvent();
        event->handleEvent();
    }
    
    if ((triggered_events & EPOLLERR) && error_event != nullptr) {
        auto* event = error_event;
        removeErrorEvent();
        event->handleEvent();
    }
    
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    if ((triggered_events & EPOLLTIMER) && timer_event != nullptr) {
        auto* event = timer_event;
        removeTimerEvent();
        event->handleEvent();
    }
#endif
}

} // namespace galay

