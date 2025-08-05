#ifndef __GALAY_BASE_H__
#define __GALAY_BASE_H__

#if defined(__linux__)
struct GHandle {
    static GHandle invalid() { return GHandle{}; }
    int fd = -1;
};
#elif defined(__APPLE__)
struct GHandle {
    static GHandle invalid() { return GHandle{}; }
    int fd = -1;
};
#elif defined(WIN32) || defined(_WIN32) || defined(_WIN32_) || defined(WIN64) || defined(_WIN64) || defined(_WIN64_)
#include <WinSock2.h>
#pragma comment(lib,"ws2_32.lib")
struct GHandle {
    SOCKET fd;
};
typedef int socklen_t;
typedef signed long ssize_t;

#else
#error "Unsupported platform"
#endif

#include <concurrentqueue/moodycamel/blockingconcurrentqueue.h>

#if defined(__linux__)
    #include <linux/version.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #if LINUX_VERSION_CODE >= KERNEL_VERSION(5,5,0) && !defined(ENABLE_DEFAULT_USE_EPOLL)
        #define USE_IOURING
    #else
        #define USE_EPOLL
    #endif
#elif defined(WIN32) || defined(_WIN32) || defined(_WIN32_) || defined(WIN64) || defined(_WIN64) || defined(_WIN64_)
    #define USE_IOCP
    #define close(x) closesocket(x)
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__)
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #define USE_KQUEUE
#else
    #error "Unsupported platform"
#endif

#if defined(USE_EPOLL)
    #include <sys/epoll.h>
#elif defined(USE_IOURING)
    #include <liburing.h>
#elif defined(USE_IOCP)

#elif defined(USE_KQUEUE)
    #include <sys/event.h>
#endif
#define GALAY_EXTERN_API __attribute__((visibility("default")))

enum EventType
{
    kEventTypeNone,
    kEventTypeError,
    kEventTypeRead,
    kEventTypeWrite,
    kEventTypeTimer,
};

enum TimerManagerType
{
    kTimerManagerTypePriorityQueue = 0,
    kTimerManagerTypeRbTree,
    kTimerManagerTypeTimeWheel,
};

#ifndef GALAY_VERSION
#define GALAY_VERSION "0.0.2"
#endif

#ifndef NO_USE
#define NO_USE(x) (void)(x)
#endif

#include <utility>
#include <string>


template <class F>
class DeferClass {
    public:
    DeferClass(F&& f) : m_func(std::forward<F>(f)) {}
    DeferClass(const F& f) : m_func(f) {}
    ~DeferClass() { m_func(); }

    DeferClass(const DeferClass& e) = delete;
    DeferClass& operator=(const DeferClass& e) = delete;

private:
    F m_func;
};

#define _CONCAT(a, b) a##b
#define _MAKE_DEFER_(line) DeferClass _CONCAT(defer_placeholder, line) = [&]()

#undef DEFER
#define DEFER _MAKE_DEFER_(__LINE__)

#endif