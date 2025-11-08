#include "EventScheduler.h"
#include "EventEngine.h"
#include "galay/common/Common.h"
#include "galay/common/Log.h"
#include "Event.h"
#include <pthread.h>

namespace galay{ 

    EventScheduler::EventScheduler(int64_t fds_init_size)
        :m_fds(fds_init_size)
    {
    #if defined(USE_EPOLL)
        m_engine = std::make_shared<details::EpollEventEngine>();
    #elif defined(USE_IOURING)
        m_engine = std::make_shared<details::IOUringEventEngine>();
    #elif defined(USE_KQUEUE)
        m_engine = std::make_shared<details::KqueueEventEngine>();
    #endif
    }

    EventScheduler::EventScheduler(engine_ptr engine, int64_t fds_init_size)
        : m_fds(fds_init_size), m_engine(engine)
    {
    }

    bool EventScheduler::activeEvent(details::Event* event, void* ctx)
    {
        int fd = event->getHandle().fd;
        EventType type = event->getEventType();
        
        std::shared_ptr<EventDispatcher> dispatcher;
        bool fd_exists = m_fds.find(fd, dispatcher);
        
        // 记录操作前的状态
        uint8_t old_state = fd_exists ? dispatcher->getRegisteredEvents() : 0;
        
        // 添加对应类型的事件到 dispatcher
        if (fd_exists) {
            // fd已存在，更新事件
            m_fds.update_fn(fd, [event, type](std::shared_ptr<EventDispatcher>& d) {
                if (type == EventType::kEventTypeRead) {
                    d->addReadEvent(event);
                } else if (type == EventType::kEventTypeWrite) {
                    d->addWriteEvent(event);
                } else if (type == EventType::kEventTypeError) {
                    d->addErrorEvent(event);
                }
            });
        } else {
            // fd不存在，创建新的 dispatcher
            auto new_dispatcher = std::make_shared<EventDispatcher>();
            if (type == EventType::kEventTypeRead) {
                new_dispatcher->addReadEvent(event);
            } else if (type == EventType::kEventTypeWrite) {
                new_dispatcher->addWriteEvent(event);
            } else if (type == EventType::kEventTypeError) {
                new_dispatcher->addErrorEvent(event);
            }
            dispatcher = new_dispatcher;
            m_fds.insert(fd, new_dispatcher);
        }
        
        // 重新获取 dispatcher（可能在 update_fn 中被修改）
        if (fd_exists) {
            m_fds.find(fd, dispatcher);
        }
        
        // 将 dispatcher 设置到 event 中
        event->setDispatcher(dispatcher.get());
        
        // 判断是第一次添加还是修改
        bool is_first_event = (old_state == 0);
        
        if (is_first_event) {
            // 第一次注册这个fd，ctx 保留给其他参数（如 Timer 的 during_time）
            return m_engine->addEvent(event, ctx) == 0;
        } else {
            // fd已存在其他类型的事件，需要修改
            return m_engine->modEvent(event, ctx) == 0;
        }
    }

    bool EventScheduler::removeEvent(details::Event* event, void* ctx)
    {
        int fd = event->getHandle().fd;
        EventType type = event->getEventType();
        
        std::shared_ptr<EventDispatcher> dispatcher;
        if (!m_fds.find(fd, dispatcher)) {
            return false;  // fd不存在
        }
        
        // 从 dispatcher 中移除对应类型的事件
        if (type == EventType::kEventTypeRead) {
            m_fds.update_fn(fd, [](std::shared_ptr<EventDispatcher>& d) {
                d->removeReadEvent();
            });
        } else if (type == EventType::kEventTypeWrite) {
            m_fds.update_fn(fd, [](std::shared_ptr<EventDispatcher>& d) {
                d->removeWriteEvent();
            });
        } else if (type == EventType::kEventTypeError) {
            m_fds.update_fn(fd, [](std::shared_ptr<EventDispatcher>& d) {
                d->removeErrorEvent();
            });
        } else {
            return false;
        }
        
        // 重新获取状态检查是否为空
        m_fds.find(fd, dispatcher);
        
        if (dispatcher->isEmpty()) {
            // 检测到所有事件都清空，从 epoll/kqueue 和 map 中删除
            m_fds.erase(fd);
            return m_engine->delEvent(event, ctx) == 0;
        } else {
            // 还有其他类型的事件，需要更新监听类型
            // ctx 保留给其他参数
            return m_engine->modEvent(event, ctx) == 0;
        }
    }

    void EventScheduler::registerOnceLoopCallback(const std::function<void()>& callback)
    {
        m_engine->registerOnceLoopCallback(callback);
    }

    bool EventScheduler::start(int timeout)
    {
        if (m_engine->isRunning()) {
            return false;
        }        
        this->m_thread = std::make_unique<std::thread>([this, timeout](){
            setThreadName("EventScheduler");
            m_engine->start(timeout);
            LogTrace("[{}({}) exist successfully]", name(), m_engine->getEngineID());
        });
        return true;
    }

    bool EventScheduler::stop()
    {
        if(!m_engine->isRunning()) return false;
        m_engine->stop();
        if(m_thread->joinable()) m_thread->join();
        return true;
    }

    bool EventScheduler::notify()
    {
        return m_engine->notify();
    }

    bool EventScheduler::isRunning() const
    {
        return m_engine->isRunning();
    }

    std::optional<CommonError> EventScheduler::getError() const
    {
        return m_engine->getError();
    }

}