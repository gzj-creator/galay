#ifndef GALAY_EVENT_DISPATCH_H
#define GALAY_EVENT_DISPATCH_H

#include <atomic>
#include <cstdint>

namespace galay {
    namespace details {
        class Event;
    }
}

namespace galay {

/**
 * @brief 事件分发器，管理同一个fd的多个事件（读/写/错误）
 * @details 使用原子变量保证线程安全，支持同时注册多种事件类型
 *          当事件触发时，移除对应状态位并调用对应Event的handleEvent方法
 */
class EventDispatcher {
public:
    EventDispatcher() = default;
    ~EventDispatcher() = default;
    
    // 禁止拷贝和移动（因为包含 std::atomic）
    EventDispatcher(const EventDispatcher&) = delete;
    EventDispatcher& operator=(const EventDispatcher&) = delete;
    EventDispatcher(EventDispatcher&&) = delete;
    EventDispatcher& operator=(EventDispatcher&&) = delete;
    
    enum EventMask : uint8_t {
        None  = 0,
        Read  = 1 << 0,  // 0b0001 - 读事件
        Write = 1 << 1,  // 0b0010 - 写事件
        Error = 1 << 2,  // 0b0100 - 错误事件
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
        Timer = 1 << 3   // 0b1000 - 定时器事件（仅 macOS/BSD）
#endif
    };
    
    // 添加事件（原子操作）
    void addReadEvent(details::Event* event);
    void addWriteEvent(details::Event* event);
    void addErrorEvent(details::Event* event);
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    void addTimerEvent(details::Event* event);
#endif
    
    // 移除事件（原子操作）
    void removeReadEvent();
    void removeWriteEvent();
    void removeErrorEvent();
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    void removeTimerEvent();
#endif
    
    // 查询状态
    bool hasRead() const { 
        return registered_events.load(std::memory_order_acquire) & Read; 
    }
    
    bool hasWrite() const { 
        return registered_events.load(std::memory_order_acquire) & Write; 
    }
    
    bool hasError() const { 
        return registered_events.load(std::memory_order_acquire) & Error; 
    }
    
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    bool hasTimer() const {
        return registered_events.load(std::memory_order_acquire) & Timer;
    }
#endif
    
    bool isEmpty() const { 
        return registered_events.load(std::memory_order_acquire) == None; 
    }
    
    uint8_t getRegisteredEvents() const {
        return registered_events.load(std::memory_order_acquire);
    }
    
    // 获取事件指针
    details::Event* getReadEvent() const { return read_event; }
    details::Event* getWriteEvent() const { return write_event; }
    details::Event* getErrorEvent() const { return error_event; }
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    details::Event* getTimerEvent() const { return timer_event; }
#endif
    
    /**
     * @brief 分发事件到对应的Event并移除状态
     * @param triggered_events 触发的事件标志（epoll的events或kqueue转换后的标志）
     * @details 此方法会：
     *          1. 检查哪些事件被触发
     *          2. 原子地移除对应的状态位
     *          3. 调用对应Event的handleEvent方法
     */
    void dispatch(uint32_t triggered_events);
    
private:
    std::atomic<uint8_t> registered_events{0};  // 注册的事件类型（原子）
    details::Event* read_event = nullptr;       // 等待读事件的Event
    details::Event* write_event = nullptr;      // 等待写事件的Event
    details::Event* error_event = nullptr;      // 等待错误事件的Event
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    details::Event* timer_event = nullptr;      // 等待定时器事件的Event（仅 macOS/BSD）
#endif
};

} // namespace galay

#endif // GALAY_EVENT_DISPATCH_H

