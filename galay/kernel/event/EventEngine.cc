#include "EventEngine.h"
#if defined(__linux__)
    #include <sys/eventfd.h>
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    
#endif
#include "Event.h"
#include "EventDispatch.h"
#include "galay/common/Error.h"
#include "galay/common/Log.h"


namespace galay::details
{

    std::atomic_uint32_t EventEngine::gEngineId = 0;

    EventEngine::EventEngine()
        :m_id(0)
    {
        gEngineId.fetch_add(1);
        m_id.store(gEngineId.load());
    }

#if defined(USE_EPOLL)
    EpollEventEngine::EpollEventEngine(uint32_t max_events)
    {
        using namespace error;
        m_error.reset();
        this->m_handle.fd = 0;
        this->m_event_size = max_events;
        this->m_events = static_cast<epoll_event*>(calloc(max_events, sizeof(epoll_event)));
        this->m_stop = true;
        this->m_handle.fd = epoll_create(1);
        if(this->m_handle.fd < 0) {
            m_error = CommonError(error::CallEpollCreateError, static_cast<uint32_t>(errno));
        }
    }

    bool 
    EpollEventEngine::start(int timeout)
    {
        m_error.reset();
        bool initial = true;
        do
        {
            int nEvents;
            if(initial) {
                initial = false;
                LogTrace("[Engine start, Engine: {}]", m_handle.fd);
                this->m_stop = false;
                nEvents = epoll_wait(m_handle.fd, m_events, m_event_size, timeout);
            }else{
                nEvents = epoll_wait(m_handle.fd, m_events, m_event_size, timeout);
            }
            if(nEvents < 0) {
                continue;
            };
            for(int i = 0; i < nEvents; ++i)
            {
                if (m_events[i].data.ptr == nullptr) continue;
                
                // data.ptr 存储的是 EventDispatcher*
                EventDispatcher* dispatcher = static_cast<EventDispatcher*>(m_events[i].data.ptr);
                
                // 调用 dispatch 方法，会移除状态并调用对应Event的handleEvent
                dispatcher->dispatch(m_events[i].events);
            }
            if(! m_once_loop_cbs.empty() ) {
                for(auto& callback: m_once_loop_cbs) {
                    callback();
                }
            }
        } while (!this->m_stop);
        return true;
    }


    bool EpollEventEngine::stop()
    {
        m_error.reset();
        if(this->m_stop.load() == false)
        {
            this->m_stop.store(true);
            return notify();
        }
        return false;
    }

    bool EpollEventEngine::notify()
    {
        using namespace error;
        m_error.reset();
        GHandle handle{
            .fd = eventfd(0, EFD_NONBLOCK | EFD_SEMAPHORE | EFD_CLOEXEC)
        };
        CallbackEvent* event = new CallbackEvent(handle, EventType::kEventTypeRead, [](Event *event, [[maybe_unused]] CallbackEvent::EventDeletor deletor) {
            eventfd_t val;
            eventfd_read(event->getHandle().fd, &val);
            close(event->getHandle().fd);
        });
        addEvent(event, nullptr);
        int ret = eventfd_write(handle.fd, 1);
        if(ret < 0) {
            m_error = CommonError(error::CallEventWriteError, static_cast<uint32_t>(errno));
            return false;
        }
        return true;
    }

    int 
    EpollEventEngine::addEvent(Event *event, void* ctx)
    {
        using namespace error;
        m_error.reset();
        int fd = event->getHandle().fd;
        EventType type = event->getEventType();
        LogTrace("[Add {} To Engine({}), Handle: {}, Type: {}]]", event->name(), getEngineID(), fd, toString(type));
        
        // 查找或创建 EventDispatcher
        std::shared_ptr<EventDispatcher> dispatcher;
        bool fd_exists = m_dispatchers.find(fd, dispatcher);
        uint8_t old_state = fd_exists ? dispatcher->getRegisteredEvents() : 0;
        
        if (!fd_exists) {
            // 创建新的 dispatcher
            dispatcher = std::make_shared<EventDispatcher>();
            m_dispatchers.insert(fd, dispatcher);
        }
        
        // 添加事件到 dispatcher
        if (type == EventType::kEventTypeRead) {
            dispatcher->addReadEvent(event);
        } else if (type == EventType::kEventTypeWrite) {
            dispatcher->addWriteEvent(event);
        } else if (type == EventType::kEventTypeError) {
            dispatcher->addErrorEvent(event);
        }
        
        // 设置 event 的 dispatcher
        event->setDispatcher(dispatcher.get());
        
        // 判断是第一次添加还是修改
        bool is_first_event = (old_state == 0);
        
        if (is_first_event) {
            // 第一次注册这个fd
            epoll_event ev;
            if(!convertToEpollEvent(ev, event, ctx)) {
                return 0;
            }
            // data.ptr 存储 EventDispatcher*
            ev.data.ptr = dispatcher.get();
            
            int ret = epoll_ctl(m_handle.fd, EPOLL_CTL_ADD, fd, &ev);
            if( ret != 0 ){
                m_error = CommonError(ErrorCode::CallActiveEventError, static_cast<uint32_t>(errno));
            }
            return ret;
        } else {
            // fd已存在其他类型的事件，需要修改
            return modEvent(event, ctx);
        }
    }

    int 
    EpollEventEngine::modEvent(Event* event, void* ctx)
    {
        using namespace error;
        m_error.reset();
        int fd = event->getHandle().fd;
        LogTrace("[Mod {} In Engine({}), Handle: {}, Type: {}]]", event->name(), getEngineID(), fd, toString(event->getEventType()));
        
        // 查找 EventDispatcher
        std::shared_ptr<EventDispatcher> dispatcher;
        if (!m_dispatchers.find(fd, dispatcher)) {
            LogError("modEvent: dispatcher not found for fd {}", fd);
            return -1;
        }
        
        // 构建包含所有注册事件的 epoll_event
        epoll_event ev;
        ev.events = EPOLLET;  // ET mode
        
        if (dispatcher->hasRead()) {
            ev.events |= EPOLLIN;
        }
        if (dispatcher->hasWrite()) {
            ev.events |= EPOLLOUT;
        }
        if (dispatcher->hasError()) {
            ev.events |= EPOLLERR;
        }
        
        // data.ptr 存储 EventDispatcher*
        ev.data.ptr = dispatcher.get();
        
        int ret = epoll_ctl(m_handle.fd, EPOLL_CTL_MOD, fd, &ev);
        if( ret != 0 ) {
            m_error = CommonError(ErrorCode::CallActiveEventError, static_cast<uint32_t>(errno));
        }
        return ret;
    }

    int 
    EpollEventEngine::delEvent(Event* event, [[maybe_unused]] void* ctx)
    {
        using namespace error;
        m_error.reset();
        int fd = event->getHandle().fd;
        EventType type = event->getEventType();
        LogTrace("[Del {} From Engine({}), Handle: {}, Type: {}]]", event->name(), getEngineID(), fd, toString(type));
        
        // 查找 EventDispatcher
        std::shared_ptr<EventDispatcher> dispatcher;
        if (!m_dispatchers.find(fd, dispatcher)) {
            LogError("delEvent: dispatcher not found for fd {}", fd);
            return -1;
        }
        
        // 从 dispatcher 中移除事件
        if (type == EventType::kEventTypeRead) {
            dispatcher->removeReadEvent();
        } else if (type == EventType::kEventTypeWrite) {
            dispatcher->removeWriteEvent();
        } else if (type == EventType::kEventTypeError) {
            dispatcher->removeErrorEvent();
        }
        
        // 判断是完全删除还是修改
        if (dispatcher->isEmpty()) {
            // 所有事件都清空，从 map 和 epoll 中删除
            m_dispatchers.erase(fd);
            
            epoll_event ev;
            ev.data.ptr = dispatcher.get();
            ev.events = (EPOLLIN | EPOLLOUT | EPOLLERR);
            int ret = epoll_ctl(m_handle.fd, EPOLL_CTL_DEL, fd, &ev);
            if( ret != 0 ) {
                m_error = CommonError(ErrorCode::CallRemoveEventError, static_cast<uint32_t>(errno));
            }
            return ret;
        } else {
            // 还有其他类型的事件，需要更新监听类型
            return modEvent(event, ctx);
        }
    }

    EpollEventEngine::~EpollEventEngine()
    {
        if(m_handle.fd > 0) close(m_handle.fd);
        free(m_events);
    }

    bool 
    EpollEventEngine::convertToEpollEvent(epoll_event &ev, Event *event, [[maybe_unused]] void* ctx)
    {
        EventType event_type = event->getEventType();
        ev.events = 0;
        ev.events = EPOLLONESHOT;
        
        // 通过 event 获取关联的 EventDispatcher
        EventDispatcher* dispatcher = event->getDispatcher();
        if (dispatcher != nullptr && (event_type == kEventTypeRead || event_type == kEventTypeWrite || event_type == kEventTypeError)) {
            // 根据 dispatcher 中注册的事件类型设置监听
            if (dispatcher->hasRead()) {
                ev.events |= EPOLLIN;
            }
            if (dispatcher->hasWrite()) {
                ev.events |= EPOLLOUT;
            }
            if (dispatcher->hasError()) {
                ev.events |= EPOLLERR;
            }
            ev.events |= EPOLLET;
            return true;
        }
        
        // 否则按原有单一事件类型处理（用于Timer等特殊事件，ctx 保留给其他参数）
        switch(event_type)
        {
            case kEventTypeNone:
                return false;
            case kEventTypeError:
            {
                ev.events |= EPOLLERR;
                ev.events |= EPOLLET;
            }
                break;
            case kEventTypeRead:
            {
                ev.events |= EPOLLIN;
                ev.events |= EPOLLET;
            }
                break;
            case kEventTypeWrite:
            {
                ev.events |= EPOLLOUT;
                ev.events |= EPOLLET;
            }
                break;
            case kEventTypeTimer:
            {
                ev.events |= EPOLLIN;
                ev.events |= EPOLLET;
            }
                break;
        }
        return true;
    }

#elif defined(USE_IOURING)
    

#elif defined(USE_KQUEUE)

    KqueueEventEngine::KqueueEventEngine(uint32_t max_events)
    {
        using namespace error;
        m_error.reset();
        m_handle.fd = kqueue();
        m_event_size = max_events;
        m_events = static_cast<struct kevent*>(calloc(max_events, sizeof(struct kevent)));
        this->m_stop = true;
        if(this->m_handle.fd < 0) {
            m_error = CommonError(error::CallKqueueCreateError, static_cast<uint32_t>(errno));
        }
    }

    bool KqueueEventEngine::start(int timeout)
    {
        m_error.reset();
        bool initial = true;
        timespec ts{};
        if(timeout > 0) {
            ts.tv_sec = timeout / 1000;
            ts.tv_nsec = (timeout % 1000) * 1000000;
        }
        do
        {
            int nEvents;
            if(initial) {
                initial = false;
                LogTrace("[Engine Start, Engine: {}]", m_handle.fd);
                this->m_stop = false;
                if(timeout > 0) nEvents = kevent(m_handle.fd, nullptr, 0, m_events, m_event_size, &ts);
                else nEvents = kevent(m_handle.fd, nullptr, 0, m_events, m_event_size, nullptr);
            }else{
                if(timeout > 0) nEvents = kevent(m_handle.fd, nullptr, 0, m_events, m_event_size, &ts);
                else nEvents = kevent(m_handle.fd, nullptr, 0, m_events, m_event_size, nullptr);
            }
            if(nEvents < 0) {
                continue;
            };
            for(int i = 0; i < nEvents; ++i)
            {
                if (m_events[i].udata == nullptr) continue;
                
                // udata 始终是 EventDispatcher*
                EventDispatcher* dispatcher = static_cast<EventDispatcher*>(m_events[i].udata);
                
                // 将 kqueue 的 filter 转换为标准的事件标志
                uint32_t events = 0;
                if (m_events[i].filter == EVFILT_READ) events |= 0x01;  // EPOLLIN
                if (m_events[i].filter == EVFILT_WRITE) events |= 0x02; // EPOLLOUT
                if (m_events[i].filter == EVFILT_TIMER) events |= 0x08; // EPOLLTIMER
                
                // 调用 dispatch 方法，会移除状态并调用对应Event的handleEvent
                dispatcher->dispatch(events);
            }
            if(! m_once_loop_cbs.empty() ) {
                for(auto& callback: m_once_loop_cbs) {
                    callback();
                }
            }
        } while (!this->m_stop);
        return true;
    }

    bool KqueueEventEngine::stop()
    {
        m_error.reset();
        if(this->m_stop.load() == false)
        {
            this->m_stop.store(true);
            return notify();
        }
        return false;
    }

    bool KqueueEventEngine::notify()
    {
        m_error.reset();
        int pipefd[2];
        if(pipe(pipefd) < 0) {
            return false;
        }
        int readfd = pipefd[0];
        int writefd = pipefd[1];
        GHandle handle = {readfd};
        CallbackEvent* event = new CallbackEvent(handle, EventType::kEventTypeRead, [](Event *event, CallbackEvent::EventDeletor deletor) {
                char t;
                read(event->getHandle().fd, &t, 1);
                close(event->getHandle().fd);
            });
        addEvent(event, nullptr);
        char t = '\0';
        int ret = write(writefd, &t, 1);
        close(writefd);
        if(ret <= 0) {
            return false;
        }
        return true;
    }

    int KqueueEventEngine::addEvent(Event *event, void* ctx)
    {
        using namespace error;
        m_error.reset();
        int fd = event->getHandle().fd;
        EventType type = event->getEventType();
        LogInfo("[Add {} To Engine({}), Handle: {}, Type: {}]]", event->name(), getEngineID(), fd, toString(type));
        
        // 查找或创建 EventDispatcher
        std::shared_ptr<EventDispatcher> dispatcher;
        bool fd_exists = m_dispatchers.find(fd, dispatcher);
        uint8_t old_state = fd_exists ? dispatcher->getRegisteredEvents() : 0;
        
        if (!fd_exists) {
            // 创建新的 dispatcher
            dispatcher = std::make_shared<EventDispatcher>();
            m_dispatchers.insert(fd, dispatcher);
        }
        
        // 添加事件到 dispatcher
        if (type == EventType::kEventTypeRead) {
            dispatcher->addReadEvent(event);
        } else if (type == EventType::kEventTypeWrite) {
            dispatcher->addWriteEvent(event);
        } else if (type == EventType::kEventTypeError) {
            dispatcher->addErrorEvent(event);
        } else if (type == EventType::kEventTypeTimer) {
            dispatcher->addTimerEvent(event);
        }
        
        // 设置 event 的 dispatcher
        event->setDispatcher(dispatcher.get());
        
        // 判断是第一次添加还是修改
        bool is_first_event = (old_state == 0);
        
        LogInfo("[addEvent] fd: {}, old_state: {}, is_first: {}, hasRead: {}, hasWrite: {}", 
                 fd, old_state, is_first_event, dispatcher->hasRead(), dispatcher->hasWrite());
        
        if (is_first_event) {
            // 第一次注册这个fd
            struct kevent k_event;
            k_event.flags = EV_ADD;
            if(!convertToKEvent(k_event, event, ctx)) {
                return 0;
            }
            // udata 始终存储 EventDispatcher*
            k_event.udata = dispatcher.get();
            
            int ret = kevent(m_handle.fd, &k_event, 1, nullptr, 0, nullptr);
            if(ret != 0){
                m_error = CommonError(ErrorCode::CallActiveEventError, static_cast<uint32_t>(errno));
            }
            return ret;
        } else {
            // fd已存在其他类型的事件，需要修改（注册所有事件类型）
            LogInfo("[addEvent] calling modEvent for fd: {}", fd);
            return modEvent(event, ctx);
        }
    }

    int KqueueEventEngine::modEvent(Event *event, void* ctx)
    {
        using namespace error;
        m_error.reset();
        int fd = event->getHandle().fd;
        EventType type = event->getEventType();
        LogInfo("[Mod {} In Engine({}), Handle: {}, Type: {}]]", event->name(), getEngineID(), fd, toString(type));
        
        // 查找 EventDispatcher
        std::shared_ptr<EventDispatcher> dispatcher;
        if (!m_dispatchers.find(fd, dispatcher)) {
            // dispatcher 不存在，这不应该发生
            LogError("modEvent: dispatcher not found for fd {}", fd);
            return -1;
        }
        
        // 添加新事件到 dispatcher
        if (type == EventType::kEventTypeRead) {
            dispatcher->addReadEvent(event);
        } else if (type == EventType::kEventTypeWrite) {
            dispatcher->addWriteEvent(event);
        } else if (type == EventType::kEventTypeError) {
            dispatcher->addErrorEvent(event);
        } else if (type == EventType::kEventTypeTimer) {
            dispatcher->addTimerEvent(event);
        }
        
        // 设置 event 的 dispatcher
        event->setDispatcher(dispatcher.get());
        
        // Kqueue 需要分别注册 READ、WRITE 和 TIMER filter
        struct kevent k_events[4];  // 最多4个：读、写、错误、定时器
        int event_count = 0;
        
        if (dispatcher->hasRead()) {
            k_events[event_count].ident = fd;
            k_events[event_count].filter = EVFILT_READ;
            k_events[event_count].flags = EV_ADD | EV_CLEAR | EV_ONESHOT | EV_ENABLE;
            k_events[event_count].fflags = 0;
            k_events[event_count].data = 0;
            k_events[event_count].udata = dispatcher.get();  // 存储 EventDispatcher*
            event_count++;
        }
        
        if (dispatcher->hasWrite()) {
            k_events[event_count].ident = fd;
            k_events[event_count].filter = EVFILT_WRITE;
            k_events[event_count].flags = EV_ADD | EV_CLEAR | EV_ONESHOT | EV_ENABLE;
            k_events[event_count].fflags = 0;
            k_events[event_count].data = 0;
            k_events[event_count].udata = dispatcher.get();  // 存储 EventDispatcher*
            event_count++;
        }
        
        if (dispatcher->hasTimer()) {
            k_events[event_count].ident = fd;
            k_events[event_count].filter = EVFILT_TIMER;
            k_events[event_count].flags = EV_ADD | EV_ONESHOT | EV_ENABLE;
            k_events[event_count].fflags = 0;
            if (ctx != nullptr) {
                int64_t during_time = *static_cast<int64_t*>(ctx);
                k_events[event_count].data = during_time;
            }
            k_events[event_count].udata = dispatcher.get();  // 存储 EventDispatcher*
            event_count++;
        }
        
        // Kqueue 不直接支持 EPOLLERR，错误会通过 EV_ERROR 标志返回
        
        LogInfo("[modEvent] fd: {}, registering {} events (hasRead: {}, hasWrite: {}, hasTimer: {})", 
                fd, event_count, dispatcher->hasRead(), dispatcher->hasWrite(), dispatcher->hasTimer());
        
        int ret = kevent(m_handle.fd, k_events, event_count, nullptr, 0, nullptr);
        if(ret != 0){
            LogError("[modEvent] kevent failed for fd: {}, errno: {}", fd, errno);
            m_error = CommonError(ErrorCode::CallActiveEventError, static_cast<uint32_t>(errno));
        }
        return ret;
    }

    int KqueueEventEngine::delEvent(Event *event, void* ctx)
    {
        using namespace error;
        m_error.reset();
        int fd = event->getHandle().fd;
        EventType type = event->getEventType();
        LogInfo("[Del {} From Engine({}), Handle: {}, Type: {}]]", event->name(), getEngineID(), fd, toString(type));
        
        // 查找 EventDispatcher
        std::shared_ptr<EventDispatcher> dispatcher;
        if (!m_dispatchers.find(fd, dispatcher)) {
            LogError("delEvent: dispatcher not found for fd {}", fd);
            return -1;
        }
        
        // 从 dispatcher 中移除事件
        if (type == EventType::kEventTypeRead) {
            dispatcher->removeReadEvent();
        } else if (type == EventType::kEventTypeWrite) {
            dispatcher->removeWriteEvent();
        } else if (type == EventType::kEventTypeError) {
            dispatcher->removeErrorEvent();
        } else if (type == EventType::kEventTypeTimer) {
            dispatcher->removeTimerEvent();
        }
        
        // 判断是完全删除还是修改
        if (dispatcher->isEmpty()) {
            // 所有事件都清空，从 map 和 kqueue 中删除
            m_dispatchers.erase(fd);
            
            // 删除 kevent
            struct kevent k_event;
            k_event.flags = EV_DELETE;
            if(!convertToKEvent(k_event, event, ctx)) {
                return 0;
            }
            int ret = kevent(m_handle.fd, &k_event, 1, nullptr, 0, nullptr);
            if(ret != 0){
                m_error = CommonError(error::CallRemoveEventError, static_cast<uint32_t>(errno));
            }
            return ret;
        } else {
            // 还有其他类型的事件，需要更新监听类型
            return modEvent(event, ctx);
        }
    }

    KqueueEventEngine::~KqueueEventEngine()
    {
        if(m_handle.fd > 0) close(m_handle.fd);
        free(m_events);
    }

    bool KqueueEventEngine::convertToKEvent(struct kevent &ev, Event *event, void *ctx)
    {
        ev.ident = event->getHandle().fd;
        ev.udata = event;
        ev.flags |= (EV_CLEAR | EV_ONESHOT | EV_ENABLE);
        ev.data = 0;
        ev.fflags = 0;
        switch (event->getEventType())
        {
        case kEventTypeError:
            return false;
        case kEventTypeNone:
            return false;
        case kEventTypeRead:
            ev.filter = EVFILT_READ;
            break;
        case kEventTypeWrite:
            ev.filter = EVFILT_WRITE;
            break;
        case kEventTypeTimer:
        {
            ev.filter = EVFILT_TIMER;
            if(ctx != nullptr) {
                int64_t during_time = *static_cast<int64_t*>(ctx);
                ev.data = during_time;
            }
            
        }
            break;
        }
        return true;
    }

    #endif

    

}