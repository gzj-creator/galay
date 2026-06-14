/**
 * @file defn.hpp
 * @brief 平台检测、句柄类型和 I/O 事件掩码
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 中心配置头文件，功能包括：
 * - 检测目标平台（Linux、macOS、Windows）
 * - 验证 Linux 上恰好选择了一个 I/O 后端（epoll 或 io_uring）
 * - 定义 GHandle，平台相关的文件描述符/套接字包装器
 * - 定义 IOEventType，所有支持 I/O 操作的位掩码枚举
 * - 提供平台无关的 galay_close() 用于关闭句柄
 */

#ifndef GALAY_KERNEL_DEFN_H
#define GALAY_KERNEL_DEFN_H


// 平台检测与配置
#include <cstdint>

#if defined(__linux__)
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>

    // Linux 后端必须由构建系统显式指定，以防止库与下游翻译单元之间的宏不匹配。
    #if defined(USE_EPOLL) && defined(USE_IOURING)
        #error "USE_EPOLL 和 USE_IOURING 同时定义。请只选择一个后端。"
    #endif
    #if !defined(USE_EPOLL) && !defined(USE_IOURING)
        #error "未定义 Linux 后端宏。请通过 galay-kernel CMake 目标构建/链接，或显式传递 -DUSE_EPOLL/-DUSE_IOURING。"
    #endif

    /**
     * @brief Linux 平台句柄包装器
     * @details 包装 POSIX 文件描述符。invalid() 返回 fd = -1 的哨兵值。
     */
    struct GHandle {
        static GHandle invalid() { return GHandle{}; }  ///< 返回无效句柄哨兵值
        int fd = -1;  ///< 底层文件描述符

        bool operator==(const GHandle& other) const { return fd == other.fd; }  ///< 比较两个句柄是否相等
    };

    inline int galay_close(int fd) { return ::close(fd); }

    #include <sys/epoll.h>

#elif defined(__APPLE__)
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #ifndef USE_KQUEUE
        #define USE_KQUEUE
    #endif

    /**
     * @brief macOS/BSD 平台句柄包装器
     * @details 包装 POSIX 文件描述符。invalid() 返回 fd = -1 的哨兵值。
     */
    struct GHandle {
        static GHandle invalid() { return GHandle{}; }  ///< 返回无效句柄哨兵值
        int fd = -1;  ///< 底层文件描述符

        bool operator==(const GHandle& other) const { return fd == other.fd; }  ///< 比较两个句柄是否相等
    };

    inline int galay_close(int fd) { return ::close(fd); }

    #include <sys/event.h>

#elif defined(WIN32) || defined(_WIN32) || defined(_WIN32_) || defined(WIN64) || defined(_WIN64) || defined(_WIN64_)
    #include <WinSock2.h>
    #pragma comment(lib,"ws2_32.lib")

    /**
     * @brief Windows 平台句柄包装器
     * @details 包装 SOCKET 句柄。invalid() 返回 INVALID_SOCKET 哨兵值。
     */
    struct GHandle {
        static GHandle invalid() { return GHandle{INVALID_SOCKET}; }  ///< 返回无效句柄哨兵值
        SOCKET fd = INVALID_SOCKET;  ///< 底层套接字句柄

        bool operator==(const GHandle& other) const { return fd == other.fd; }  ///< 比较两个句柄是否相等
    };

    inline int galay_close(SOCKET fd) { return closesocket(fd); }

    // Windows 特定类型定义
    typedef int socklen_t;
    typedef signed long ssize_t;

#else
    #error "不支持的平台"
#endif

    /**
     * @brief I/O 事件类型位掩码
     * @details 标识控制器当前等待的事件。值可通过按位 OR 组合。
     */
    enum IOEventType: uint32_t {
        INVALID     = 0,       ///< 无效 / 无事件
        ACCEPT      = 1u << 0, ///< 等待 accept()
        CONNECT     = 1u << 1, ///< 等待 connect()
        RECV        = 1u << 2, ///< 等待 recv()
        SEND        = 1u << 3, ///< 等待 send()
        READV       = 1u << 4, ///< Scatter-gather 读（readv）
        WRITEV      = 1u << 5, ///< Scatter-gather 写（writev）
        SENDFILE    = 1u << 6, ///< 零拷贝文件发送（sendfile）
        FILEREAD    = 1u << 7, ///< 文件读等待
        FILEWRITE   = 1u << 8, ///< 文件写等待
        FILEWATCH   = 1u << 9, ///< 文件监控等待
        RECVFROM    = 1u << 10,///< 等待 recvfrom()
        SENDTO      = 1u << 11,///< 等待 sendto()
        SEQUENCE    = 1u << 12,///< 复合顺序等待器
    };

    inline IOEventType operator|(IOEventType a, IOEventType b) {  ///< 组合两个事件位掩码
        return static_cast<IOEventType>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }
    inline IOEventType operator&(IOEventType a, IOEventType b) {  ///< 交集两个事件位掩码
        return static_cast<IOEventType>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
    }
    inline IOEventType operator~(IOEventType a) {  ///< 事件掩码按位取反
        return static_cast<IOEventType>(~static_cast<uint32_t>(a));
    }
    inline IOEventType& operator|=(IOEventType& a, IOEventType b) {  ///< 原地组合事件位掩码
        a = a | b; return a;
    }
    inline IOEventType& operator&=(IOEventType& a, IOEventType b) {  ///< 原地交集事件位掩码
        a = a & b; return a;
    }

#endif
