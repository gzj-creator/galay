#include "EventDispatch.h"
#include "Event.h"

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

void EventDispatcher::dispatch(uint32_t triggered_events)
{
    // 根据触发的事件类型，移除状态并调用对应的Event
    // 注意：多个事件可能同时触发（例如：同时可读可写）
    
    if ((triggered_events & EPOLLIN) && read_event != nullptr) {
        // 先保存指针，避免在 removeReadEvent 后变为 nullptr
        auto* event = read_event;
        // 移除读事件状态
        removeReadEvent();
        // 调用Event的handleEvent方法
        event->handleEvent();
    }
    
    if ((triggered_events & EPOLLOUT) && write_event != nullptr) {
        // 先保存指针，避免在 removeWriteEvent 后变为 nullptr
        auto* event = write_event;
        // 移除写事件状态
        removeWriteEvent();
        // 调用Event的handleEvent方法
        event->handleEvent();
    }
    
    if ((triggered_events & EPOLLERR) && error_event != nullptr) {
        // 先保存指针，避免在 removeErrorEvent 后变为 nullptr
        auto* event = error_event;
        // 移除错误事件状态
        removeErrorEvent();
        // 调用Event的handleEvent方法
        event->handleEvent();
    }
}

} // namespace galay

