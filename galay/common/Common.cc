#include "Common.h"
#if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
    #include <sys/socket.h>
    #include <arpa/inet.h>
    #include <fcntl.h>
#ifdef __linux__
     #include <sys/sendfile.h>
    #include <sys/eventfd.h>
#endif
#elif defined(WIN32) || defined(_WIN32) || defined(_WIN32_) || defined(WIN64) || defined(_WIN64) || defined(_WIN64_)
    #include <ws2ipdef.h>
    #include <WS2tcpip.h>
#endif

namespace galay 
{

    HandleOption::HandleOption(const GHandle handle)
        : m_handle(handle)
    {
    }

    std::expected<void, CommonError> HandleOption::handleBlock()
    {
        using namespace error;
    #if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
        int flag = fcntl(m_handle.fd, F_GETFL, 0);
        flag &= ~O_NONBLOCK;
        int ret = fcntl(m_handle.fd, F_SETFL, flag);
    #elif defined(WIN32) || defined(_WIN32) || defined(_WIN32_) || defined(WIN64) || defined(_WIN64) || defined(_WIN64_)
        u_long mode = 0; // 1 表示非阻塞模式
        int ret = ioctlsocket(m_handle, FIONBIO, &mode);
    #endif
        if (ret < 0) {
            return std::unexpected(CommonError{CallSetBlockError, static_cast<uint32_t>(errno)});
        }
        return {};
    }

    std::expected<void, CommonError> HandleOption::handleNonBlock()
    {
        using namespace error;
    #if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
        int flag = fcntl(m_handle.fd, F_GETFL, 0);
        flag |= O_NONBLOCK;
        int ret = fcntl(m_handle.fd, F_SETFL, flag);
    #elif defined(WIN32) || defined(_WIN32) || defined(_WIN32_) || defined(WIN64) || defined(_WIN64) || defined(_WIN64_)
        u_long mode = 1; // 1 表示非阻塞模式
        int ret = ioctlsocket(m_handle.fd, FIONBIO, &mode);
    #endif
        if (ret < 0) {
            return std::unexpected(CommonError(CallSetNoBlockError, static_cast<uint32_t>(errno)));
        }
        return {};
    }

    std::expected<void, CommonError> HandleOption::handleReuseAddr()
    {
        using namespace error;
    #if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
        int option = 1;
        int ret = setsockopt(m_handle.fd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));
    #elif  defined(WIN32) || defined(_WIN32) || defined(_WIN32_) || defined(WIN64) || defined(_WIN64) || defined(_WIN64_)
        BOOL option = TRUE;
        int ret = setsockopt(m_handle.fd, SOL_SOCKET, SO_REUSEADDR, (char*)&option, sizeof(option));
    #endif
        if (ret < 0) {
            return std::unexpected(CommonError(CallSetSockOptError, static_cast<uint32_t>(errno)));
        }
        return {};
    }

    std::expected<void, CommonError> HandleOption::handleReusePort()
    {
        using namespace error;
    #if defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
        constexpr int option = 1;
        if (const int ret = setsockopt(m_handle.fd, SOL_SOCKET, SO_REUSEPORT, &option, sizeof(option)); ret < 0) {
            return std::unexpected(CommonError(CallSetSockOptError, static_cast<uint32_t>(errno)));
        }
    #elif  defined(WIN32) || defined(_WIN32) || defined(_WIN32_) || defined(WIN64) || defined(_WIN64) || defined(_WIN64_)
        //To Do
    #endif
        return {};
    }
}