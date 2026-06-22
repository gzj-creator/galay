/**
 * @file async_file.h
 * @brief 基于 kqueue 和 io_uring 后端的协程友好异步文件 I/O
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 构建在 IO 调度器的可等待类型之上的简单异步文件抽象。
 * 支持 open、read、write、close、size 和 sync 操作。
 * 仅在使用 USE_KQUEUE 或 USE_IOURING 编译时可用。
 * 对于基于 epoll 的系统，请使用 AioFile。
 */

#ifndef GALAY_ASYNC_FILE_H
#define GALAY_ASYNC_FILE_H

// AsyncFile 仅用于 kqueue (macOS) 和 io_uring (Linux)
// epoll 平台请使用 AioFile

#if defined(USE_KQUEUE) || defined(USE_IOURING)

#include "../core/io_scheduler.hpp"
#include "../core/awaitable.h"
#include "../common/error.h"
#include <expected>
#include <string>
#include <fcntl.h>

namespace galay::async
{
/**
 * @brief 标准异步文件操作的文件打开模式
 *
 * @details 与 AioFile 不同，这些模式不强制使用 O_DIRECT。
 * 支持只读、只写、读写、追加和截断模式。
 */
enum class FileOpenMode : int {
    Read      = O_RDONLY,  ///< 只读
    Write     = O_WRONLY | O_CREAT,  ///< 只写，必要时创建文件
    ReadWrite = O_RDWR | O_CREAT,  ///< 读写，必要时创建文件
    Append    = O_WRONLY | O_CREAT | O_APPEND,  ///< 追加写
    Truncate  = O_WRONLY | O_CREAT | O_TRUNC,  ///< 打开时截断文件
};

/**
 * @brief 基于 kqueue 和 io_uring 后端的协程友好异步文件
 *
 * @details 封装 IOController，通过调度器的可等待类型提供独立的异步读、写和关闭操作。
 * 不支持 AioFile 使用的批量 preRead/preWrite/commit 模式。
 *
 * @note 对于基于 epoll 的系统，请使用 AioFile。
 */
class AsyncFile
{
public:
    /**
     * @brief 默认构造函数；以无效句柄初始化
     */
    AsyncFile();

    /**
     * @brief 析构函数；同步关闭仍由对象持有的文件
     */
    ~AsyncFile();

    AsyncFile(const AsyncFile&) = delete;
    AsyncFile& operator=(const AsyncFile&) = delete;

    /**
     * @brief 移动构造函数；从 other 转移 IO 控制器
     * @param other 被移动的对象
     */
    AsyncFile(AsyncFile&& other) noexcept;

    /**
     * @brief 移动赋值运算符；关闭当前句柄后再转移
     * @param other 被移动的对象
     * @return 当前对象的引用
     */
    AsyncFile& operator=(AsyncFile&& other) noexcept;

    /**
     * @brief 以指定模式打开给定路径的文件
     *
     * @param path 文件系统路径
     * @param mode 打开模式（Read、Write、ReadWrite、Append、Truncate）
     * @param permissions 文件创建权限（默认 0644）
     * @return 成功返回 void，失败返回 IOError
     * @note 若对象已持有文件，成功打开新文件前会先同步关闭旧 fd。
     */
    std::expected<void, galay::kernel::IOError> open(
        const std::string& path,
        FileOpenMode mode,
        int permissions = 0644);

    /**
     * @brief 异步读取文件
     *
     * @param buffer 读取缓冲区
     * @param length 缓冲区大小
     * @param offset 文件偏移
     * @return FileReadAwaitable 可等待对象，co_await 后返回读取到的字节数
     *
     * @note
     * - 返回值为0表示 EOF
     * - 缓冲区生命周期必须持续到 co_await 完成
     */
    galay::kernel::FileReadAwaitable read(char* buffer, size_t length, off_t offset = 0);  ///< 异步读取文件，恢复后返回实际读取字节数

    /**
     * @brief 异步写入数据到文件
     *
     * @param buffer 源数据缓冲区
     * @param length 要写入的字节数
     * @param offset 文件偏移量（默认 0）
     * @return FileWriteAwaitable，解析为写入的字节数
     *
     * @note 缓冲区生命周期必须持续到 co_await 完成
     */
    galay::kernel::FileWriteAwaitable write(const char* buffer, size_t length, off_t offset = 0);

    /**
     * @brief 异步关闭文件句柄
     * @return CloseAwaitable，关闭操作完成时解析
     */
    galay::kernel::CloseAwaitable close();

    /**
     * @brief 获取底层文件句柄
     * @return 当前的 GHandle
     */
    GHandle handle() const { return m_controller.m_handle; }

    /**
     * @brief 通过 fstat 获取当前文件大小
     * @return 成功时返回文件大小（字节），失败时返回 IOError
     */
    std::expected<size_t, galay::kernel::IOError> size() const;

    /**
     * @brief 通过 fsync 将文件数据刷入稳定存储
     * @return 成功返回 void，失败返回 IOError
     */
    std::expected<void, galay::kernel::IOError> sync();

    /**
     * @brief 获取内部 IO 控制器（用于高级操作）
     * @return IOController 指针
     */
    galay::kernel::IOController* getController() { return &m_controller; }

private:
    galay::kernel::IOController m_controller;
};

} // namespace galay::async

#endif // USE_KQUEUE || USE_IOURING

#endif // GALAY_ASYNC_FILE_H
