/**
 * @file file_watcher.cc
 * @brief 异步文件和目录变更监控实现
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details FileWatcher 的平台特定实现：
 * - Linux (io_uring/epoll)：使用 inotify 进行事件监控
 * - macOS (kqueue)：使用文件描述符和 EVFILT_VNODE
 *
 * 包含在 Linux 上将通用 FileWatchEvent 位掩码转换为平台原生 inotify 掩码的辅助函数。
 */

#include "file_watcher.h"
#include "galay-kernel/common/defn.hpp"

#if defined(USE_IOURING) || defined(USE_EPOLL) || defined(USE_KQUEUE)

#include <unistd.h>
#include <fcntl.h>
#include <cerrno>

#if defined(USE_IOURING) || defined(USE_EPOLL)
#include <sys/inotify.h>
#endif

#ifdef USE_KQUEUE
// O_EVTONLY 是 macOS 专用标志，仅用于事件通知，不允许读写
#ifndef O_EVTONLY
#define O_EVTONLY 0x8000
#endif
#endif

namespace galay::async
{

using namespace galay::kernel;

/**
 * @brief 将通用 FileWatchEvent 位掩码转换为 Linux inotify 掩码
 * @param events FileWatchEvent 值的位掩码
 * @return 对应的 inotify 事件掩码
 */
#if defined(USE_IOURING) || defined(USE_EPOLL)
static uint32_t toInotifyMask(FileWatchEvent events)
{
    uint32_t mask = 0;
    uint32_t e = static_cast<uint32_t>(events);
    if (e & static_cast<uint32_t>(FileWatchEvent::Access))       mask |= IN_ACCESS;
    if (e & static_cast<uint32_t>(FileWatchEvent::Modify))       mask |= IN_MODIFY;
    if (e & static_cast<uint32_t>(FileWatchEvent::Attrib))       mask |= IN_ATTRIB;
    if (e & static_cast<uint32_t>(FileWatchEvent::CloseWrite))   mask |= IN_CLOSE_WRITE;
    if (e & static_cast<uint32_t>(FileWatchEvent::CloseNoWrite)) mask |= IN_CLOSE_NOWRITE;
    if (e & static_cast<uint32_t>(FileWatchEvent::Open))         mask |= IN_OPEN;
    if (e & static_cast<uint32_t>(FileWatchEvent::MovedFrom))    mask |= IN_MOVED_FROM;
    if (e & static_cast<uint32_t>(FileWatchEvent::MovedTo))      mask |= IN_MOVED_TO;
    if (e & static_cast<uint32_t>(FileWatchEvent::Create))       mask |= IN_CREATE;
    if (e & static_cast<uint32_t>(FileWatchEvent::Delete))       mask |= IN_DELETE;
    if (e & static_cast<uint32_t>(FileWatchEvent::DeleteSelf))   mask |= IN_DELETE_SELF;
    if (e & static_cast<uint32_t>(FileWatchEvent::MoveSelf))     mask |= IN_MOVE_SELF;
    return mask;
}
#endif

/**
 * @brief 构造 FileWatcher；在 Linux 上初始化 inotify，在 macOS 上延迟初始化
 */
FileWatcher::FileWatcher()
    : m_controller(GHandle::invalid())
#ifdef USE_KQUEUE
    , m_current_events(FileWatchEvent::None)
#endif
{
#if defined(USE_IOURING) || defined(USE_EPOLL)
    m_controller.m_handle.fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
#endif
    // macOS: m_controller 在 addWatch 时设置为第一个监控的文件 fd
}

/**
 * @brief 析构函数；移除所有监控并关闭所有文件描述符
 */
FileWatcher::~FileWatcher()
{
#if defined(USE_IOURING) || defined(USE_EPOLL)
    if (m_controller.m_handle.fd >= 0) {
        for (const auto& [wd, path] : m_watches) {
            inotify_rm_watch(m_controller.m_handle.fd, wd);
        }
        close(m_controller.m_handle.fd);
    }
#else
    // macOS: 关闭所有打开的文件描述符
    for (const auto& [fd, path] : m_watches) {
        close(fd);
    }
#endif
}

/**
 * @brief 移动构造函数；从 other 转移监控状态
 * @param other 被移动的对象
 */
FileWatcher::FileWatcher(FileWatcher&& other) noexcept
    : m_controller(std::move(other.m_controller))
    , m_watches(std::move(other.m_watches))
#ifdef USE_KQUEUE
    , m_current_events(other.m_current_events)
#endif
{
}

/**
 * @brief 移动赋值运算符；清理当前状态后再转移
 * @param other 被移动的对象
 * @return 当前对象的引用
 */
FileWatcher& FileWatcher::operator=(FileWatcher&& other) noexcept
{
    if (this != &other) {
        // 清理当前资源
#if defined(USE_IOURING) || defined(USE_EPOLL)
        if (m_controller.m_handle.fd >= 0) {
            for (const auto& [wd, path] : m_watches) {
                inotify_rm_watch(m_controller.m_handle.fd, wd);
            }
            close(m_controller.m_handle.fd);
        }
#else
        for (const auto& [fd, path] : m_watches) {
            close(fd);
        }
#endif

        // 移动资源
        m_controller = std::move(other.m_controller);
        m_watches = std::move(other.m_watches);
#ifdef USE_KQUEUE
        m_current_events = other.m_current_events;
#endif
    }
    return *this;
}

/**
 * @brief 注册路径以进行监控
 *
 * @details 在 Linux 上使用 inotify_add_watch。在 macOS 上以 O_EVTONLY 打开文件，
 * 并存储 fd 用于 kqueue EVFILT_VNODE 监控。
 *
 * @param path 要监控的文件或目录路径
 * @param events 要监控的事件类型
 * @return 监控描述符（Linux：inotify wd，macOS：文件 fd），或 IOError
 */
std::expected<int, IOError> FileWatcher::addWatch(const std::string& path, FileWatchEvent events)
{
#if defined(USE_IOURING) || defined(USE_EPOLL)
    if (m_controller.m_handle.fd < 0) {
        return std::unexpected(IOError(kOpenFailed, EBADF));
    }

    uint32_t mask = toInotifyMask(events);
    int wd = inotify_add_watch(m_controller.m_handle.fd, path.c_str(), mask);
    if (wd < 0) {
        return std::unexpected(IOError(kOpenFailed, errno));
    }

    m_watches[wd] = path;
    return wd;
#else
    // macOS: 打开文件获取 fd，用于 kqueue EVFILT_VNODE
    int fd = open(path.c_str(), O_RDONLY | O_EVTONLY);
    if (fd < 0) {
        return std::unexpected(IOError(kOpenFailed, errno));
    }

    m_watches[fd] = path;
    m_current_events = events;

    // 设置第一个监控的 fd 作为 watch_fd
    if (m_controller.m_handle.fd < 0) {
        m_controller.m_handle.fd = fd;
    }

    return fd;
#endif
}

/**
 * @brief 通过描述符移除先前注册的监控
 *
 * @details 在 Linux 上调用 inotify_rm_watch。在 macOS 上关闭文件描述符，
 * 如果被移除的 fd 是活跃的则更新控制器。
 *
 * @param wd 要移除的监控描述符
 * @return 成功返回 void，描述符无效或移除失败时返回 IOError
 */
std::expected<void, IOError> FileWatcher::removeWatch(int wd)
{
#if defined(USE_IOURING) || defined(USE_EPOLL)
    if (m_controller.m_handle.fd < 0) {
        return std::unexpected(IOError(kOpenFailed, EBADF));
    }

    auto it = m_watches.find(wd);
    if (it == m_watches.end()) {
        return std::unexpected(IOError(kOpenFailed, EINVAL));
    }

    if (inotify_rm_watch(m_controller.m_handle.fd, wd) < 0) {
        return std::unexpected(IOError(kOpenFailed, errno));
    }

    m_watches.erase(it);
    return {};
#else
    // macOS: 关闭文件描述符
    auto it = m_watches.find(wd);
    if (it == m_watches.end()) {
        return std::unexpected(IOError(kOpenFailed, EINVAL));
    }

    close(wd);
    m_watches.erase(it);

    // 如果删除的是当前 watch_fd，更新为下一个
    if (wd == m_controller.m_handle.fd) {
        if (!m_watches.empty()) {
            m_controller.m_handle.fd = m_watches.begin()->first;
        } else {
            m_controller.m_handle.fd = -1;
        }
    }

    return {};
#endif
}

/**
 * @brief 创建一个可等待对象，挂起直到文件系统事件到达
 *
 * @details 在 macOS 上，将当前事件掩码传递给可等待对象用于 kqueue EVFILT_VNODE 过滤器设置。
 * 在 Linux 上，直接使用 inotify fd。
 *
 * @return 绑定到该监控器控制器的 FileWatchAwaitable
 */
FileWatchAwaitable FileWatcher::watch()
{
#ifdef USE_KQUEUE
    return FileWatchAwaitable(&m_controller,
                              m_buffer, BUFFER_SIZE, m_current_events);
#else
    return FileWatchAwaitable(&m_controller,
                              m_buffer, BUFFER_SIZE);
#endif
}

/**
 * @brief 查找与监控描述符关联的路径
 * @param wd 监控描述符
 * @return 注册的路径字符串，未找到时返回空字符串
 */
std::string FileWatcher::getPath(int wd) const
{
    auto it = m_watches.find(wd);
    if (it != m_watches.end()) {
        return it->second;
    }
    return {};
}

} // namespace galay::async

#endif // USE_IOURING || USE_EPOLL || USE_KQUEUE
