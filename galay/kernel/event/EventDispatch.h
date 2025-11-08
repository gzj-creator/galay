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
        Read  = 1 << 0,  // 0b001 - 读事件
        Write = 1 << 1,  // 0b010 - 写事件
        Error = 1 << 2   // 0b100 - 错误事件
    };
    
    // 添加事件（原子操作）
    void addReadEvent(details::Event* event);
    void addWriteEvent(details::Event* event);
    void addErrorEvent(details::Event* event);
    
    // 移除事件（原子操作）
    void removeReadEvent();
    void removeWriteEvent();
    void removeErrorEvent();
    
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
    
    bool isEmpty() const { 
        return registered_events.load(std::memory_order_acquire) == None; 
    }
    
    uint8_t getRegisteredEvents() const {
        return registered_events.load(std::memory_order_acquire);
    }
    
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
};

} // namespace galay

#endif // GALAY_EVENT_DISPATCH_H

