/**
 * @file file_descriptor.h
 * @brief RAII 文件描述符管理
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 提供 POSIX 文件描述符的独占所有权封装，自动在析构时关闭有效 fd。
 *          支持移动语义、显式关闭、释放所有权以及打开失败的 IOError 返回。
 */

#ifndef GALAY_KERNEL_FILE_DESCRIPTOR_H
#define GALAY_KERNEL_FILE_DESCRIPTOR_H

#include "galay-kernel/common/error.h"

#include <cerrno>
#include <expected>
#include <fcntl.h>
#include <optional>
#include <sys/types.h>
#include <unistd.h>
#include <utility>

namespace galay::kernel
{

/**
 * @brief RAII 文件描述符管理类
 * @details 该类型独占一个 POSIX 文件描述符，析构时自动关闭；不可拷贝，可移动。
 * @note open/read/write 等系统调用本身是同步接口，本类不提供异步调度语义。
 */
class FileDescriptor
{
public:
    /**
     * @brief 构造无效文件描述符。
     */
    FileDescriptor() noexcept
        : m_fd(-1)
    {
    }

    /**
     * @brief 构造对象并尝试打开文件。
     * @param path 文件路径，调用期间必须指向有效的 C 字符串
     * @param flags 传给 ::open 的打开标志
     * @note 打开失败不会抛异常，可通过 valid()/lastError() 或 open() 返回值处理。
     */
    FileDescriptor(const char* path, int flags)
        : m_fd(-1)
    {
        (void)open(path, flags);
    }

    /**
     * @brief 构造对象并尝试打开文件。
     * @param path 文件路径，调用期间必须指向有效的 C 字符串
     * @param flags 传给 ::open 的打开标志
     * @param mode 创建新文件时使用的权限
     * @note 打开失败不会抛异常，可通过 valid()/lastError() 或 open() 返回值处理。
     */
    FileDescriptor(const char* path, int flags, mode_t mode)
        : m_fd(-1)
    {
        (void)open(path, flags, mode);
    }

    /**
     * @brief 析构时关闭当前持有的有效文件描述符。
     */
    ~FileDescriptor() noexcept
    {
        close();
    }

    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    /**
     * @brief 移动构造，接管 other 持有的文件描述符。
     * @param other 被接管的对象，返回后变为无效描述符状态
     */
    FileDescriptor(FileDescriptor&& other) noexcept
        : m_fd(other.m_fd)
        , m_last_error(std::move(other.m_last_error))
    {
        other.m_fd = -1;
    }

    /**
     * @brief 移动赋值，先关闭当前 fd，再接管 other 的 fd。
     * @param other 被接管的对象，返回后变为无效描述符状态
     * @return 当前对象引用
     */
    FileDescriptor& operator=(FileDescriptor&& other) noexcept
    {
        if (this != &other) {
            close();
            m_fd = other.m_fd;
            m_last_error = std::move(other.m_last_error);
            other.m_fd = -1;
        }
        return *this;
    }

    /**
     * @brief 打开文件并接管返回的文件描述符。
     * @param path 文件路径，调用期间必须指向有效的 C 字符串
     * @param flags 传给 ::open 的打开标志
     * @return 成功返回 void，失败返回 IOError(kOpenFailed)，并保持对象无效。
     */
    std::expected<void, IOError> open(const char* path, int flags)
    {
        close();
        m_fd = ::open(path, flags);
        if (m_fd < 0) {
            m_last_error = IOError(kOpenFailed, errno);
            return std::unexpected(*m_last_error);
        }
        m_last_error.reset();
        return {};
    }

    /**
     * @brief 打开文件并接管返回的文件描述符。
     * @param path 文件路径，调用期间必须指向有效的 C 字符串
     * @param flags 传给 ::open 的打开标志
     * @param mode 创建新文件时使用的权限
     * @return 成功返回 void，失败返回 IOError(kOpenFailed)，并保持对象无效。
     */
    std::expected<void, IOError> open(const char* path, int flags, mode_t mode)
    {
        close();
        m_fd = ::open(path, flags, mode);
        if (m_fd < 0) {
            m_last_error = IOError(kOpenFailed, errno);
            return std::unexpected(*m_last_error);
        }
        m_last_error.reset();
        return {};
    }

    /**
     * @brief 若当前持有有效 fd，则同步关闭它。
     */
    void close() noexcept
    {
        if (m_fd >= 0) {
            ::close(m_fd);
            m_fd = -1;
        }
    }

    /**
     * @brief 获取当前文件描述符。
     * @return 有效 fd，或 -1 表示无效
     */
    int get() const noexcept
    {
        return m_fd;
    }

    /**
     * @brief 检查当前是否持有有效文件描述符。
     * @return 持有有效 fd 返回 true
     */
    bool valid() const noexcept
    {
        return m_fd >= 0;
    }

    /**
     * @brief 获取最近一次 open 失败的错误。
     * @return 若最近一次 open 失败则返回 IOError，否则返回 std::nullopt
     */
    const std::optional<IOError>& lastError() const noexcept
    {
        return m_last_error;
    }

    /**
     * @brief 释放文件描述符所有权，不关闭底层 fd。
     * @return 原始 fd，若当前无效则返回 -1
     */
    int release() noexcept
    {
        int fd = m_fd;
        m_fd = -1;
        return fd;
    }

    /**
     * @brief 交换两个对象持有的文件描述符。
     * @param other 另一个 FileDescriptor 对象
     */
    void swap(FileDescriptor& other) noexcept
    {
        std::swap(m_fd, other.m_fd);
    }

    /**
     * @brief 显式转换为 bool，等价于 valid()。
     */
    explicit operator bool() const noexcept
    {
        return valid();
    }

    /**
     * @brief 获取当前文件描述符。
     * @return 有效 fd，或 -1 表示无效
     */
    int operator*() const noexcept
    {
        return m_fd;
    }

private:
    int m_fd;
    std::optional<IOError> m_last_error;
};

} // namespace galay::kernel

#endif // GALAY_KERNEL_FILE_DESCRIPTOR_H
