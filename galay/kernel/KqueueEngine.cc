#include "Engine.h"
#include "Waker.h"
#include "common/Base.h"
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include "Coroutine.hpp"
#include "kernel/Timer.h"

#if defined(USE_KQUEUE)

namespace galay 
{

KqueueEngine::KqueueEngine(uint32_t max_events)
    : m_stop(true)
    , m_event_size(max_events)
    , m_last_error(0)
    , m_notify_read_fd(-1)
    , m_notify_write_fd(-1)
    , m_timer_manager(std::make_shared<PriorityQueueTimerManager>())
{
    m_events = new struct kevent[max_events];

    // 创建kqueue实例
    m_handle.fd = kqueue();
    if (m_handle.fd == -1) {
        m_last_error = errno;
        return;
    }

    // 创建用于通知的pipe
    int pipe_fds[2];
    if (pipe(pipe_fds) == -1) {
        m_last_error = errno;
        close(m_handle.fd);
        m_handle.fd = -1;
        return;
    }

    // 设置非阻塞
    fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK);
    fcntl(pipe_fds[1], F_SETFL, O_NONBLOCK);

    // 注册读端到kqueue
    struct kevent kev;
    EV_SET(&kev, pipe_fds[0], EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
    if (kevent(m_handle.fd, &kev, 1, nullptr, 0, nullptr) == -1) {
        m_last_error = errno;
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        close(m_handle.fd);
        m_handle.fd = -1;
        throw std::runtime_error("[notify handle(pipe register failed)]");
    }

    // 保存通知pipe的fd
    m_notify_read_fd = pipe_fds[0];
    m_notify_write_fd = pipe_fds[1];
}

KqueueEngine::~KqueueEngine() {
    stop();

    if (m_events) {
        delete[] m_events;
    }

    if (m_notify_read_fd != -1) {
        close(m_notify_read_fd);
    }
    if (m_notify_write_fd != -1) {
        close(m_notify_write_fd);
    }
    if (m_handle.fd != -1) {
        close(m_handle.fd);
    }
}

bool KqueueEngine::start(int timeout) {
    if (m_handle.fd == -1 || timeout <= 0) {
        return false;
    }
    m_stop = false;

    struct kevent* events = m_events;

    struct timespec ts;
    struct timespec* timeout_ptr = nullptr;
    if (timeout >= 0) {
        // 使用最大100ms的超时来平衡响应性和性能
        int poll_timeout = std::min(timeout, PollMaxTimeout);
        ts.tv_sec = poll_timeout / 1000;
        ts.tv_nsec = (poll_timeout % 1000) * 1000000;
        timeout_ptr = &ts;
    }

    CoroutineBase::wptr batch_ready_coroutine[BatchCoroutineDequeueSize];
    size_t count = 0;

    while (!m_stop.load(std::memory_order_relaxed)) {
        // 从调度队列取出任务
        do {
            count = m_ready_queue.try_dequeue_bulk(batch_ready_coroutine, BatchCoroutineDequeueSize);
            for(auto i = 0; i < count; ++i) {
                auto co_ptr = batch_ready_coroutine[i].lock();
                if (co_ptr) {
                    // 检查协程状态，只执行未完成的协程
                    if (!co_ptr->isDone() && !co_ptr->isDestroying()) {
                        co_ptr->resume();
                    }
                }
                // weak_ptr 失效表示协程已被销毁，自动跳过
            }
        } while(count != 0);
        // 等待I/O事件
        int nfds = kevent(m_handle.fd, nullptr, 0, events, m_event_size, timeout_ptr);

        if (nfds == -1) {
            // EINTR 是可中断的系统调用，通常应该继续
            if (errno == EINTR) {
                continue;
            }
            // EWOULDBLOCK 和 EAGAIN 表示没有事件，但在有超时的情况下是正常的
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                continue;
            }
            // 其他错误是真正的错误
            m_last_error = errno;
            return false;
        }

        // 处理I/O事件
        for (int i = 0; i < nfds; ++i) {
            struct kevent* ev = &events[i];

            // 检查是否是通知事件
            if (ev->ident == static_cast<uintptr_t>(m_notify_read_fd)) {
                // 读取通知数据
                char buf[32];
                while (read(m_notify_read_fd, buf, sizeof(buf)) > 0) {
                }
                continue;
            }

            // 定时事件
            if(ev->filter == EVFILT_TIMER && ev->ident == TIMER_IDEL) {
                tick();
            }

            // 处理其他事件 - udata应该是WakerWrapper
            if (ev->udata) {
                // 根据事件类型触发对应的waker
                if (ev->filter == EVFILT_READ) {
                    // 触发读事件的waker
                    WakerWrapper* wrapper = static_cast<WakerWrapper*>(ev->udata);
                    if (wrapper->contains(WakerType::READ)) {
                        wrapper->getWaker(WakerType::READ)->wakeUp(ev);
                    }
                } else if (ev->filter == EVFILT_WRITE) {
                    // 触发写事件的waker
                    WakerWrapper* wrapper = static_cast<WakerWrapper*>(ev->udata);
                    if (wrapper->contains(WakerType::WRITE)) {
                        wrapper->getWaker(WakerType::WRITE)->wakeUp(ev);
                    }
                } else if (ev->filter == EVFILT_VNODE) {
                    // 文件事件
                    WakerWrapper* wrapper = static_cast<WakerWrapper*>(ev->udata);
                    if (wrapper->contains(WakerType::FILEWATCH)) {
                        wrapper->getWaker(WakerType::FILEWATCH)->wakeUp(ev);
                    }
                }
            }
        }
    }

    return true;
}

bool KqueueEngine::stop() {
    m_stop = true;
    return notify();
}

bool KqueueEngine::notify() {
    if (m_notify_write_fd == -1) {
        return false;
    }
    // 写入一个字节来通知
    char c = 1;
    ssize_t ret = write(m_notify_write_fd, &c, 1);
    if(ret == -1 && (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) ) {
        m_last_error = errno;
        return false;
    }
    return ret == 1 || (ret == -1 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR));
}

bool KqueueEngine::spawn(std::weak_ptr<CoroutineBase> co)
{
    if(auto co_ptr = co.lock()) {
        co_ptr->belongEngine(this);
    } else {
        return false;
    }
    // 将协程加入队列
    m_ready_queue.enqueue(co);
    return true;
}

bool KqueueEngine::spawnBatch(const std::vector<std::weak_ptr<CoroutineBase>>& cos)
{
    if(cos.empty()) {
        return false;
    }
    for(auto& co: cos) {
        if(auto co_ptr = co.lock()) {
            co_ptr->belongEngine(this);
        } else {
            return false;
        }
    }
    if(!m_ready_queue.enqueue_bulk(cos.data(), cos.size())) {
        return false;
    }

    return true;
}

uint64_t KqueueEngine::getLastError() 
{
    return m_last_error;
}


int KqueueEngine::addWakers(WakerWrapper* wrapper, WakerType type, Waker waker, GHandle handle, void *ctx) 
{
    if (!wrapper || m_handle.fd == -1) {
        return -1;
    }

    // 添加waker到wrapper
    wrapper->addType(type, std::move(waker));

    // 构造kevent - 需要从ctx获取fd
    struct kevent kev {};

    if (!convertToKEvent(kev, type, &waker, handle, ctx)) {
        return -1;
    }

    // 添加到kqueue
    if (kevent(m_handle.fd, &kev, 1, nullptr, 0, nullptr) == -1) {
        m_last_error = errno;
        return -1;
    }

    return 0;
}

int KqueueEngine::modWakers(WakerWrapper* wrapper, WakerType type, Waker waker, GHandle handle, void* ctx) 
{
    if (!wrapper || m_handle.fd == -1) {
        return -1;
    }

    // 对于kqueue，修改和添加是一样的，因为kevent可以自动处理
    return addWakers(wrapper, type, std::move(waker), handle, ctx);
}

int KqueueEngine::delWakers(WakerWrapper* wrapper, WakerType type, Waker waker, GHandle handle, void* ctx) 
{
    if (!wrapper || m_handle.fd == -1) {
        return -1;
    }

    wrapper->delType(type);

    //只有所有事件都没有才执行
    if(!wrapper->empty()) return 0;

    struct kevent kev{};

    if (!convertToKEvent(kev, type, &waker, handle, ctx)) {
        return -1;
    }

    // 设置为删除操作
    kev.flags = EV_DELETE;

    // 从kqueue删除
    if (kevent(m_handle.fd, &kev, 1, nullptr, 0, nullptr) == -1) {
        m_last_error = errno;
        return -1;
    }

    return 0;
}

Timer::ptr KqueueEngine::addTimer(std::chrono::milliseconds ms, const std::function<void()>& callback)
{
    Timer::ptr timer = std::make_shared<Timer>(ms, callback);
    //添加timermanager
    m_timer_manager->push(timer);
    auto earliest_timer = m_timer_manager->getEarliestTimer();
    if(earliest_timer.get() == timer.get()) {
        //说明需要刷新fd的时间
        struct kevent kev;
        EV_SET(&kev, static_cast<uintptr_t>(TIMER_IDEL), EVFILT_TIMER, EV_ADD | EV_ENABLE | EV_CLEAR, 0,
               static_cast<int64_t>(ms.count()), nullptr);
        kevent(m_handle.fd, &kev, 1, nullptr, 0, nullptr);
    }
    return timer;
}

void KqueueEngine::tick()
{
    //执行已到期的timer
    m_timer_manager->onTimerTick();
    auto earliest_timer= m_timer_manager->getEarliestTimer();
    if(earliest_timer) {
        //取出最早的timer的剩余时间刷新fd
        auto remain = earliest_timer->getRemainTime();
        struct kevent kev;
        EV_SET(&kev, static_cast<uintptr_t>(TIMER_IDEL), EVFILT_TIMER, EV_ADD | EV_ENABLE | EV_CLEAR, 0,
               static_cast<int64_t>(remain), nullptr);
        kevent(m_handle.fd, &kev, 1, nullptr, 0, nullptr);
    }
}


bool KqueueEngine::convertToKEvent(struct kevent &ev, WakerType type, Waker* waker, GHandle handle, void* ctx) {
    switch (type) {
        case WakerType::READ:
            EV_SET(&ev, handle.fd, EVFILT_READ, EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, waker);
            break;

        case WakerType::WRITE:
            EV_SET(&ev, handle.fd, EVFILT_WRITE, EV_ADD | EV_ENABLE | EV_CLEAR, 0, 0, waker);
            break;
        default:
            return false;
    }

    return true;
}

} // namespace galay

#endif // USE_KQUEUE