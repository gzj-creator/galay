#ifndef GALAY_EVENT_ENGINE_H
#define GALAY_EVENT_ENGINE_H
#include "galay/common/Base.h"
#include "galay/common/Error.h"
#include "EventDispatch.h"
#include <libcuckoo/cuckoohash_map.hh>
#include <string>
#include <atomic>
#include <list>
#include <memory>
#include <functional>
#include <optional>

namespace galay::details
{
    class Event;

    #define DEFAULT_MAX_EVENTS 1024

    using namespace error;

#if defined(USE_IOURING)
    // io_uring 操作类型
    enum class IOOperationType : uint8_t {
        kOpNone = 0,
        kOpRead,
        kOpWrite,
        kOpAccept,
        kOpConnect,
        kOpClose,
        kOpRecv,
        kOpSend,
        kOpRecvfrom,
        kOpSendto,
        kOpPollAdd,     // 用于兼容 Reactor 模式
        kOpTimeout,
    };

    // io_uring 请求上下文
    struct IORequest {
        IOOperationType op = IOOperationType::kOpNone;
        Event* event = nullptr;         // 关联的事件对象
        int fd = -1;
        void* buffer = nullptr;
        size_t length = 0;
        int flags = 0;
        // 用于 accept/connect
        sockaddr* addr = nullptr;
        socklen_t* addrlen = nullptr;
        // 用于 sendto/recvfrom
        sockaddr* remote_addr = nullptr;
        socklen_t remote_addrlen = 0;
        // io_uring 完成结果
        int result = 0;
    };
#endif

    class EventEngine
    {
        static std::atomic_uint32_t gEngineId;
    public:
        using ptr = std::shared_ptr<EventEngine>;
        EventEngine();
        uint32_t getEngineID() const { return m_id.load(); }
        virtual bool start(int timeout) = 0;
        virtual bool stop() = 0;
        virtual bool notify() = 0;
        virtual int addEvent(Event* event, void* ctx) = 0;
        virtual int modEvent(Event* event, void* ctx) = 0;
        virtual int delEvent(Event* event, void* ctx) = 0;
        virtual bool isRunning() const = 0;
        virtual std::optional<CommonError> getError() = 0;
        virtual GHandle getHandle() = 0;
        virtual void registerOnceLoopCallback(const std::function<void()>& callback) = 0;
        virtual ~EventEngine() = default;

#if defined(USE_IOURING)
        // Proactor 风格接口 (io_uring 专用)
        virtual int submitRead(Event* event, int fd, void* buf, size_t len) { return -1; }
        virtual int submitWrite(Event* event, int fd, const void* buf, size_t len) { return -1; }
        virtual int submitRecv(Event* event, int fd, void* buf, size_t len, int flags) { return -1; }
        virtual int submitSend(Event* event, int fd, const void* buf, size_t len, int flags) { return -1; }
        virtual int submitAccept(Event* event, int fd, sockaddr* addr, socklen_t* addrlen) { return -1; }
        virtual int submitConnect(Event* event, int fd, const sockaddr* addr, socklen_t addrlen) { return -1; }
        virtual int submitClose(Event* event, int fd) { return -1; }
        virtual int submitRecvfrom(Event* event, int fd, void* buf, size_t len, int flags,
                                   sockaddr* src_addr, socklen_t* addrlen) { return -1; }
        virtual int submitSendto(Event* event, int fd, const void* buf, size_t len, int flags,
                                 const sockaddr* dest_addr, socklen_t addrlen) { return -1; }
#endif

    protected:
        std::atomic_uint32_t m_id;
    };

    #if defined(USE_EPOLL)
    //default ET mode
    class EpollEventEngine: public EventEngine
    {
    public:
        EpollEventEngine(uint32_t max_events = DEFAULT_MAX_EVENTS);
        bool start(int timeout = -1) override;
        bool stop() override;
        bool notify() override;
        int addEvent(Event* event, void* ctx) override;
        int modEvent(Event* event, void* ctx) override;
        int delEvent(Event* event, void* ctx) override;
        bool isRunning() const override { return !m_stop; }
        std::optional<CommonError> getError() override { return m_error; }
        GHandle getHandle() override { return m_handle; }
        void registerOnceLoopCallback(const std::function<void()>& callback) override { m_once_loop_cbs.push_back(callback); }
        ~EpollEventEngine() override;
    private:
        bool convertToEpollEvent(struct epoll_event &ev, Event *event, void* ctx);
    private:
        GHandle m_handle;
        std::optional<CommonError> m_error;
        std::atomic_bool m_stop;
        std::atomic_uint32_t m_event_size;
        std::atomic<epoll_event*> m_events;
        std::list<std::function<void()>> m_once_loop_cbs;
        libcuckoo::cuckoohash_map<int, std::shared_ptr<galay::EventDispatcher>> m_dispatchers;
    };
    #elif defined(USE_IOURING)

    class IOUringEventEngine final: public EventEngine {
    public:
        explicit IOUringEventEngine(uint32_t queue_depth = DEFAULT_MAX_EVENTS);
        bool start(int timeout) override;
        bool stop() override;
        bool notify() override;
        int addEvent(Event* event, void* ctx) override;
        int modEvent(Event* event, void* ctx) override;
        int delEvent(Event* event, void* ctx) override;
        bool isRunning() const override { return !m_stop.load(); }
        std::optional<CommonError> getError() override { return m_error; }
        GHandle getHandle() override { return GHandle{.fd = m_ring.ring_fd}; }
        void registerOnceLoopCallback(const std::function<void()>& callback) override { m_once_loop_cbs.push_back(callback); }
        ~IOUringEventEngine() override;

        // Proactor 风格接口实现
        int submitRead(Event* event, int fd, void* buf, size_t len) override;
        int submitWrite(Event* event, int fd, const void* buf, size_t len) override;
        int submitRecv(Event* event, int fd, void* buf, size_t len, int flags) override;
        int submitSend(Event* event, int fd, const void* buf, size_t len, int flags) override;
        int submitAccept(Event* event, int fd, sockaddr* addr, socklen_t* addrlen) override;
        int submitConnect(Event* event, int fd, const sockaddr* addr, socklen_t addrlen) override;
        int submitClose(Event* event, int fd) override;
        int submitRecvfrom(Event* event, int fd, void* buf, size_t len, int flags,
                          sockaddr* src_addr, socklen_t* addrlen) override;
        int submitSendto(Event* event, int fd, const void* buf, size_t len, int flags,
                        const sockaddr* dest_addr, socklen_t addrlen) override;

    private:
        io_uring_sqe* getSqe();
        int submitAndWait();
        void processCompletions();

    private:
        io_uring m_ring;
        std::optional<CommonError> m_error;
        std::atomic_bool m_stop{true};
        std::list<std::function<void()>> m_once_loop_cbs;

        // 用于存储 msghdr 结构（recvfrom/sendto 需要）
        struct MsgHdrContext {
            msghdr msg;
            iovec iov;
        };
        libcuckoo::cuckoohash_map<Event*, std::unique_ptr<MsgHdrContext>> m_msg_contexts;
    };
    #elif defined(USE_SELECT)

    #elif defined(USE_KQUEUE)

    //default ET 
    class KqueueEventEngine final : public EventEngine
    {
    public:
        explicit KqueueEventEngine(uint32_t max_events = DEFAULT_MAX_EVENTS);
        bool start(int timeout = -1) override;
        bool stop() override;
        bool notify() override;
        int addEvent(Event* event, void* ctx ) override;
        int modEvent(Event* event, void* ctx) override;
        int delEvent(Event* event, void* ctx) override;
        bool isRunning() const override { return !m_stop; }
        std::optional<CommonError> getError() override { return m_error; }
        GHandle getHandle() override { return m_handle; }
        void registerOnceLoopCallback(const std::function<void()>& callback) override { m_once_loop_cbs.push_back(callback); }
        ~KqueueEventEngine() override;
    private:
        bool convertToKEvent(struct kevent &ev, Event *event, void* ctx);
    private:
        GHandle m_handle{};
        std::optional<CommonError> m_error;
        std::atomic_bool m_stop;
        std::atomic_uint32_t m_event_size;
        std::atomic<struct kevent*> m_events;
        std::list<std::function<void()>> m_once_loop_cbs;
        libcuckoo::cuckoohash_map<int, std::shared_ptr<galay::EventDispatcher>> m_dispatchers;
    };

    #endif





}

#endif