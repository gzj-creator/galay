#include "EventEngine.h"
#if defined(__linux__)
    #include <sys/eventfd.h>
    #include <poll.h>
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
                // 在纯ET模式下,事件不会自动从epoll移除,dispatcher会管理event指针
                // event的handleEvent会导致协程resume,协程可能会重新注册新的事件
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
        } else if (type == EventType::kEventTypeTimer) {
            // 在Linux上，Timer使用timerfd，产生EPOLLIN事件，所以当作Read事件处理
            dispatcher->addReadEvent(event);
        }
        
        // 设置 event 的 dispatcher
        event->setDispatcher(dispatcher.get());
        
        // 构造epoll_event
        epoll_event ev;
        if(!convertToEpollEvent(ev, event, ctx)) {
            return 0;
        }
        ev.data.ptr = dispatcher.get();
        
        // 判断是第一次添加还是修改
        // 在ET模式下,old_state==0说明之前没有事件或所有事件都已触发并移除
        bool is_first_event = (old_state == 0);
        
        int ret;
        if (is_first_event) {
            // 第一次注册,使用ADD
            ret = epoll_ctl(m_handle.fd, EPOLL_CTL_ADD, fd, &ev);
            // 如果ADD失败且错误是EEXIST,说明fd已在epoll中,改用MOD
            if (ret != 0 && errno == EEXIST) {
                LogTrace("[Add failed with EEXIST, try MOD] fd: {}", fd);
                ret = epoll_ctl(m_handle.fd, EPOLL_CTL_MOD, fd, &ev);
            }
        } else {
            // fd已有其他事件,使用MOD
            ret = epoll_ctl(m_handle.fd, EPOLL_CTL_MOD, fd, &ev);
            // 如果MOD失败且错误是ENOENT,说明fd不在epoll中(可能被dispatch后删除了),改用ADD
            if (ret != 0 && errno == ENOENT) {
                LogTrace("[MOD failed with ENOENT, try ADD] fd: {}", fd);
                ret = epoll_ctl(m_handle.fd, EPOLL_CTL_ADD, fd, &ev);
            }
        }
        
        if( ret != 0 ){
            m_error = CommonError(ErrorCode::CallActiveEventError, static_cast<uint32_t>(errno));
        } else {
            // 成功时清空errno,避免残留的错误码影响后续操作
            errno = 0;
        }
        return ret;
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
        ev.events = EPOLLET;  // ET mode (不使用ONESHOT)
        
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
        } else if (type == EventType::kEventTypeTimer) {
            // 在Linux上，Timer被当作Read事件处理
            dispatcher->removeReadEvent();
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
        // 注意: 不使用 EPOLLONESHOT，因为它会在任何事件触发后移除整个fd
        // 这与kqueue的per-filter ONESHOT行为不一致
        // 改用纯ET模式，事件触发后不自动移除，由dispatcher在处理后决定是否保持注册
        ev.events = EPOLLET;
        
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
            }
                break;
            case kEventTypeRead:
            {
                ev.events |= EPOLLIN;
            }
                break;
            case kEventTypeWrite:
            {
                ev.events |= EPOLLOUT;
            }
                break;
            case kEventTypeTimer:
            {
                ev.events |= EPOLLIN;
            }
                break;
        }
        return true;
    }

#elif defined(USE_IOURING)

    IOUringEventEngine::IOUringEventEngine(uint32_t queue_depth)
    {
        using namespace error;
        m_error.reset();
        int ret = io_uring_queue_init(queue_depth, &m_ring, 0);
        if (ret < 0) {
            m_error = CommonError(ErrorCode::CallIOUringInitError, static_cast<uint32_t>(-ret));
        }
    }

    IOUringEventEngine::~IOUringEventEngine()
    {
        io_uring_queue_exit(&m_ring);
    }

    bool IOUringEventEngine::start(int timeout)
    {
        m_error.reset();
        m_stop.store(false);
        LogTrace("[IOUring Engine start, Engine: {}]", m_ring.ring_fd);

        __kernel_timespec ts{};
        __kernel_timespec* ts_ptr = nullptr;
        if (timeout > 0) {
            ts.tv_sec = timeout / 1000;
            ts.tv_nsec = (timeout % 1000) * 1000000;
            ts_ptr = &ts;
        }

        while (!m_stop.load()) {
            io_uring_cqe* cqe;
            int ret;

            if (ts_ptr) {
                ret = io_uring_wait_cqe_timeout(&m_ring, &cqe, ts_ptr);
            } else {
                ret = io_uring_wait_cqe(&m_ring, &cqe);
            }

            if (ret < 0) {
                if (ret == -ETIME || ret == -EINTR) {
                    continue;
                }
                continue;
            }

            // 处理完成事件
            Event* event = static_cast<Event*>(io_uring_cqe_get_data(cqe));
            if (event != nullptr) {
                // 存储结果到 event（通过 IOResultHolder 接口）
                // event 需要实现 setIOResult 方法
                int result = cqe->res;

                // 清理 msghdr 上下文（如果有）
                m_msg_contexts.erase(event);

                // 调用 event 的 handleEvent，传递结果
                // 这里我们通过一个临时存储来传递结果
                // Event 子类需要在 handleEvent 之前获取结果
                if (auto* io_event = dynamic_cast<class IOResultHolder*>(event)) {
                    io_event->setIOResult(result);
                }

                event->handleEvent();
            }

            io_uring_cqe_seen(&m_ring, cqe);

            // 处理一次性回调
            if (!m_once_loop_cbs.empty()) {
                for (auto& callback : m_once_loop_cbs) {
                    callback();
                }
                m_once_loop_cbs.clear();
            }
        }

        return true;
    }

    bool IOUringEventEngine::stop()
    {
        m_error.reset();
        if (!m_stop.load()) {
            m_stop.store(true);
            return notify();
        }
        return false;
    }

    bool IOUringEventEngine::notify()
    {
        using namespace error;
        m_error.reset();

        // 使用 IORING_OP_NOP 来唤醒等待的线程
        io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
        if (!sqe) {
            m_error = CommonError(ErrorCode::CallIOUringGetSqeError, 0);
            return false;
        }

        io_uring_prep_nop(sqe);
        io_uring_sqe_set_data(sqe, nullptr);

        int ret = io_uring_submit(&m_ring);
        if (ret < 0) {
            m_error = CommonError(ErrorCode::CallIOUringSubmitError, static_cast<uint32_t>(-ret));
            return false;
        }
        return true;
    }

    // addEvent 使用 POLL_ADD 实现 Reactor 兼容
    int IOUringEventEngine::addEvent(Event* event, void* ctx)
    {
        using namespace error;
        m_error.reset();

        int fd = event->getHandle().fd;
        EventType type = event->getEventType();
        LogTrace("[Add {} To IOUring Engine({}), Handle: {}, Type: {}]", event->name(), getEngineID(), fd, toString(type));

        io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
        if (!sqe) {
            m_error = CommonError(ErrorCode::CallIOUringGetSqeError, 0);
            return -1;
        }

        // 转换事件类型到 poll 事件
        unsigned poll_mask = 0;
        switch (type) {
            case kEventTypeRead:
            case kEventTypeTimer:
                poll_mask = POLLIN;
                break;
            case kEventTypeWrite:
                poll_mask = POLLOUT;
                break;
            case kEventTypeError:
                poll_mask = POLLERR;
                break;
            default:
                return -1;
        }

        io_uring_prep_poll_add(sqe, fd, poll_mask);
        io_uring_sqe_set_data(sqe, event);

        int ret = io_uring_submit(&m_ring);
        if (ret < 0) {
            m_error = CommonError(ErrorCode::CallIOUringSubmitError, static_cast<uint32_t>(-ret));
            return ret;
        }
        return 0;
    }

    int IOUringEventEngine::modEvent(Event* event, void* ctx)
    {
        // io_uring 的 poll 是一次性的，修改需要先删除再添加
        delEvent(event, ctx);
        return addEvent(event, ctx);
    }

    int IOUringEventEngine::delEvent(Event* event, void* ctx)
    {
        using namespace error;
        m_error.reset();

        int fd = event->getHandle().fd;
        LogTrace("[Del {} From IOUring Engine({}), Handle: {}]", event->name(), getEngineID(), fd);

        io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
        if (!sqe) {
            m_error = CommonError(ErrorCode::CallIOUringGetSqeError, 0);
            return -1;
        }

        io_uring_prep_poll_remove(sqe, reinterpret_cast<__u64>(event));
        io_uring_sqe_set_data(sqe, nullptr);

        int ret = io_uring_submit(&m_ring);
        if (ret < 0) {
            m_error = CommonError(ErrorCode::CallIOUringSubmitError, static_cast<uint32_t>(-ret));
            return ret;
        }
        return 0;
    }

    io_uring_sqe* IOUringEventEngine::getSqe()
    {
        io_uring_sqe* sqe = io_uring_get_sqe(&m_ring);
        if (!sqe) {
            // 队列满了，先提交再获取
            io_uring_submit(&m_ring);
            sqe = io_uring_get_sqe(&m_ring);
        }
        return sqe;
    }

    int IOUringEventEngine::submitRead(Event* event, int fd, void* buf, size_t len)
    {
        using namespace error;
        m_error.reset();

        io_uring_sqe* sqe = getSqe();
        if (!sqe) {
            m_error = CommonError(ErrorCode::CallIOUringGetSqeError, 0);
            return -1;
        }

        io_uring_prep_read(sqe, fd, buf, len, 0);
        io_uring_sqe_set_data(sqe, event);

        int ret = io_uring_submit(&m_ring);
        if (ret < 0) {
            m_error = CommonError(ErrorCode::CallIOUringSubmitError, static_cast<uint32_t>(-ret));
            return ret;
        }
        return 0;
    }

    int IOUringEventEngine::submitWrite(Event* event, int fd, const void* buf, size_t len)
    {
        using namespace error;
        m_error.reset();

        io_uring_sqe* sqe = getSqe();
        if (!sqe) {
            m_error = CommonError(ErrorCode::CallIOUringGetSqeError, 0);
            return -1;
        }

        io_uring_prep_write(sqe, fd, buf, len, 0);
        io_uring_sqe_set_data(sqe, event);

        int ret = io_uring_submit(&m_ring);
        if (ret < 0) {
            m_error = CommonError(ErrorCode::CallIOUringSubmitError, static_cast<uint32_t>(-ret));
            return ret;
        }
        return 0;
    }

    int IOUringEventEngine::submitRecv(Event* event, int fd, void* buf, size_t len, int flags)
    {
        using namespace error;
        m_error.reset();

        io_uring_sqe* sqe = getSqe();
        if (!sqe) {
            m_error = CommonError(ErrorCode::CallIOUringGetSqeError, 0);
            return -1;
        }

        io_uring_prep_recv(sqe, fd, buf, len, flags);
        io_uring_sqe_set_data(sqe, event);

        int ret = io_uring_submit(&m_ring);
        if (ret < 0) {
            m_error = CommonError(ErrorCode::CallIOUringSubmitError, static_cast<uint32_t>(-ret));
            return ret;
        }
        return 0;
    }

    int IOUringEventEngine::submitSend(Event* event, int fd, const void* buf, size_t len, int flags)
    {
        using namespace error;
        m_error.reset();

        io_uring_sqe* sqe = getSqe();
        if (!sqe) {
            m_error = CommonError(ErrorCode::CallIOUringGetSqeError, 0);
            return -1;
        }

        io_uring_prep_send(sqe, fd, buf, len, flags);
        io_uring_sqe_set_data(sqe, event);

        int ret = io_uring_submit(&m_ring);
        if (ret < 0) {
            m_error = CommonError(ErrorCode::CallIOUringSubmitError, static_cast<uint32_t>(-ret));
            return ret;
        }
        return 0;
    }

    int IOUringEventEngine::submitAccept(Event* event, int fd, sockaddr* addr, socklen_t* addrlen)
    {
        using namespace error;
        m_error.reset();

        io_uring_sqe* sqe = getSqe();
        if (!sqe) {
            m_error = CommonError(ErrorCode::CallIOUringGetSqeError, 0);
            return -1;
        }

        io_uring_prep_accept(sqe, fd, addr, addrlen, 0);
        io_uring_sqe_set_data(sqe, event);

        int ret = io_uring_submit(&m_ring);
        if (ret < 0) {
            m_error = CommonError(ErrorCode::CallIOUringSubmitError, static_cast<uint32_t>(-ret));
            return ret;
        }
        return 0;
    }

    int IOUringEventEngine::submitConnect(Event* event, int fd, const sockaddr* addr, socklen_t addrlen)
    {
        using namespace error;
        m_error.reset();

        io_uring_sqe* sqe = getSqe();
        if (!sqe) {
            m_error = CommonError(ErrorCode::CallIOUringGetSqeError, 0);
            return -1;
        }

        io_uring_prep_connect(sqe, fd, addr, addrlen);
        io_uring_sqe_set_data(sqe, event);

        int ret = io_uring_submit(&m_ring);
        if (ret < 0) {
            m_error = CommonError(ErrorCode::CallIOUringSubmitError, static_cast<uint32_t>(-ret));
            return ret;
        }
        return 0;
    }

    int IOUringEventEngine::submitClose(Event* event, int fd)
    {
        using namespace error;
        m_error.reset();

        io_uring_sqe* sqe = getSqe();
        if (!sqe) {
            m_error = CommonError(ErrorCode::CallIOUringGetSqeError, 0);
            return -1;
        }

        io_uring_prep_close(sqe, fd);
        io_uring_sqe_set_data(sqe, event);

        int ret = io_uring_submit(&m_ring);
        if (ret < 0) {
            m_error = CommonError(ErrorCode::CallIOUringSubmitError, static_cast<uint32_t>(-ret));
            return ret;
        }
        return 0;
    }

    int IOUringEventEngine::submitRecvfrom(Event* event, int fd, void* buf, size_t len, int flags,
                                           sockaddr* src_addr, socklen_t* addrlen)
    {
        using namespace error;
        m_error.reset();

        io_uring_sqe* sqe = getSqe();
        if (!sqe) {
            m_error = CommonError(ErrorCode::CallIOUringGetSqeError, 0);
            return -1;
        }

        // 创建并存储 msghdr 上下文
        auto ctx = std::make_unique<MsgHdrContext>();
        ctx->iov.iov_base = buf;
        ctx->iov.iov_len = len;
        ctx->msg.msg_name = src_addr;
        ctx->msg.msg_namelen = addrlen ? *addrlen : 0;
        ctx->msg.msg_iov = &ctx->iov;
        ctx->msg.msg_iovlen = 1;
        ctx->msg.msg_control = nullptr;
        ctx->msg.msg_controllen = 0;
        ctx->msg.msg_flags = 0;

        io_uring_prep_recvmsg(sqe, fd, &ctx->msg, flags);
        io_uring_sqe_set_data(sqe, event);

        // 存储上下文，防止被释放
        m_msg_contexts.insert(event, std::move(ctx));

        int ret = io_uring_submit(&m_ring);
        if (ret < 0) {
            m_msg_contexts.erase(event);
            m_error = CommonError(ErrorCode::CallIOUringSubmitError, static_cast<uint32_t>(-ret));
            return ret;
        }
        return 0;
    }

    int IOUringEventEngine::submitSendto(Event* event, int fd, const void* buf, size_t len, int flags,
                                         const sockaddr* dest_addr, socklen_t addrlen)
    {
        using namespace error;
        m_error.reset();

        io_uring_sqe* sqe = getSqe();
        if (!sqe) {
            m_error = CommonError(ErrorCode::CallIOUringGetSqeError, 0);
            return -1;
        }

        // 创建并存储 msghdr 上下文
        auto ctx = std::make_unique<MsgHdrContext>();
        ctx->iov.iov_base = const_cast<void*>(buf);
        ctx->iov.iov_len = len;
        ctx->msg.msg_name = const_cast<sockaddr*>(dest_addr);
        ctx->msg.msg_namelen = addrlen;
        ctx->msg.msg_iov = &ctx->iov;
        ctx->msg.msg_iovlen = 1;
        ctx->msg.msg_control = nullptr;
        ctx->msg.msg_controllen = 0;
        ctx->msg.msg_flags = 0;

        io_uring_prep_sendmsg(sqe, fd, &ctx->msg, flags);
        io_uring_sqe_set_data(sqe, event);

        // 存储上下文，防止被释放
        m_msg_contexts.insert(event, std::move(ctx));

        int ret = io_uring_submit(&m_ring);
        if (ret < 0) {
            m_msg_contexts.erase(event);
            m_error = CommonError(ErrorCode::CallIOUringSubmitError, static_cast<uint32_t>(-ret));
            return ret;
        }
        return 0;
    }

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