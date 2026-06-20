/**
 * @file file_watcher.h
 * @brief 异步文件和目录变更监控
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 提供平台自适应的文件监控能力：
 * - Linux (io_uring/epoll)：使用 inotify 监控文件和目录事件
 * - macOS (kqueue)：使用 EVFILT_VNODE 进行逐文件监控
 *
 * 监控器对协程友好：调用 addWatch 注册路径，然后 co_await watch() 异步接收变更事件。
 *
 * 在使用 USE_IOURING、USE_EPOLL 或 USE_KQUEUE 编译时可用。
 */

#ifndef GALAY_ASYNC_FILE_WATCHER_H
#define GALAY_ASYNC_FILE_WATCHER_H


// FileWatcher 支持:
// - Linux: inotify (io_uring/epoll)
// - macOS: kqueue EVFILT_VNODE

#if defined(USE_IOURING) || defined(USE_EPOLL) || defined(USE_KQUEUE)

#include "../core/io_scheduler.hpp"
#include "../core/awaitable.h"
#include "../common/error.h"
#include <expected>
#include <string>
#include <unordered_map>

namespace galay::async
{
/**
 * @brief 异步文件监控类
 *
 * @details 监控文件或目录的变化。
 * - Linux: 使用 inotify
 * - macOS: 使用 kqueue EVFILT_VNODE
 *
 * @code
 * Task<void> watchFile() {
 *     FileWatcher watcher;
 *     auto result = watcher.addWatch("/path/to/file", FileWatchEvent::Modify);
 *     if (!result) {
 *         // 处理错误
 *         co_return;
 *     }
 *
 *     while (true) {
 *         auto event = co_await watcher.watch();
 *         if (event) {
 *             // 处理事件
 *             if (event->has(FileWatchEvent::Modify)) {
 *                 // 文件被修改
 *             }
 *         }
 *     }
 * }
 * @endcode
 */
/**
 * @brief 异步文件和目录变更监控器
 *
 * @details 监控文件系统路径的变更，如修改、创建、删除和移动。
 * 使用平台特定机制：
 * - Linux：通过 io_uring 或 epoll 使用 inotify
 * - macOS：kqueue EVFILT_VNODE
 *
 * 典型用法：
 * 1. 构造 FileWatcher
 * 2. 为每个要监控的路径调用 addWatch()
 * 3. 在循环中 co_await watch() 接收事件
 *
 * @note 不可拷贝；可移动。
 */
class FileWatcher
{
public:
    /**
     * @brief 构造 FileWatcher 并初始化平台特定的后端
     */
    FileWatcher();

    /**
     * @brief 析构函数；移除所有监控并释放资源
     */
    ~FileWatcher();

    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    /**
     * @brief 移动构造函数；转移监控状态
     * @param other 被移动的对象
     */
    FileWatcher(FileWatcher&& other) noexcept;

    /**
     * @brief 移动赋值运算符；清理当前状态后再转移
     * @param other 被移动的对象
     * @return 当前对象的引用
     */
    FileWatcher& operator=(FileWatcher&& other) noexcept;

    /**
     * @brief 注册路径以进行文件系统变更监控
     *
     * @param path 要监控的文件或目录路径
     * @param events 要监控的事件类型位掩码（默认 All）
     * @return 成功时返回监控描述符，失败时返回 IOError
     *
     * @note 在 Linux 上使用目录监控时，FileWatchResult::name 包含变更的文件名。
     *       在 macOS 上，每个文件需要单独的监控。
     */
    std::expected<int, galay::kernel::IOError> addWatch(
        const std::string& path,
        galay::kernel::FileWatchEvent events = galay::kernel::FileWatchEvent::All);

    /**
     * @brief 移除先前注册的监控
     * @param wd 由 addWatch 返回的监控描述符
     * @return 成功返回 void，描述符无效时返回 IOError
     */
    std::expected<void, galay::kernel::IOError> removeWatch(int wd);

    /**
     * @brief 异步等待下一个文件系统事件
     * @return FileWatchAwaitable，解析为 FileWatchResult 或 IOError
     */
    galay::kernel::FileWatchAwaitable watch();

    /**
     * @brief 检查监控器是否初始化成功
     * @return 如果底层文件描述符有效则返回 true
     */
    bool isValid() const { return m_watch_fd >= 0; }

    /**
     * @brief 获取底层监控文件描述符
     * @return Linux：inotify fd；macOS：第一个被监控文件的 fd
     */
    int fd() const { return m_watch_fd; }

    /**
     * @brief 查找与监控描述符关联的路径
     * @param wd 监控描述符
     * @return 注册的路径，未找到时返回空字符串
     */
    std::string getPath(int wd) const;

    /**
     * @brief 获取内部 IO 控制器（用于高级操作）
     * @return IOController 指针
     */
    galay::kernel::IOController* getController() { return &m_controller; }

private:
    int m_watch_fd;                                ///< Linux: inotify fd, macOS: 当前监控的 fd
    galay::kernel::IOController m_controller;      ///< IO控制器
    std::unordered_map<int, std::string> m_watches; ///< wd/fd -> path 映射

    static constexpr size_t BUFFER_SIZE = 4096;    ///< 事件缓冲区大小
    char m_buffer[BUFFER_SIZE];                    ///< 事件缓冲区

#ifdef USE_KQUEUE
    galay::kernel::FileWatchEvent m_current_events; ///< macOS: 当前监控的事件类型
#endif
};

} // namespace galay::async

#endif // USE_IOURING || USE_EPOLL || USE_KQUEUE

#endif // GALAY_ASYNC_FILE_WATCHER_H
