/**
 * @file file_descriptor.h
 * @brief RAII 文件描述符管理
 * @author galay-http
 * @version 1.0.0
 *
 * @details 提供 RAII 风格的文件描述符封装，自动管理文件打开/关闭生命周期，
 *          支持移动语义、所有权释放与交换操作。
 */

#ifndef GALAY_FILE_DESCRIPTOR_H
#define GALAY_FILE_DESCRIPTOR_H

#include "galay-kernel/common/error.h"
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <expected>
#include <optional>
#include <string>
#include <utility>

namespace galay::http
{

/**
 * @brief RAII 文件描述符管理类
 * @details 自动管理文件描述符的生命周期，防止资源泄漏
 */
class FileDescriptor
{
public:
    /**
     * @brief 默认构造函数
     */
    FileDescriptor() noexcept
        : m_fd(-1)
    {
    }

    /**
     * @brief 构造函数并打开文件
     * @param path 文件路径
     * @param flags 打开标志（O_RDONLY, O_WRONLY, O_RDWR 等）
     * @note 打开失败不会抛异常，可通过 valid()/lastError() 或 open() 返回值处理
     */
    FileDescriptor(const char* path, int flags)
        : m_fd(-1)
    {
        (void)open(path, flags);
    }

    /**
     * @brief 构造函数并打开文件（带权限）
     * @param path 文件路径
     * @param flags 打开标志
     * @param mode 文件权限（创建新文件时使用）
     * @note 打开失败不会抛异常，可通过 valid()/lastError() 或 open() 返回值处理
     */
    FileDescriptor(const char* path, int flags, mode_t mode)
        : m_fd(-1)
    {
        (void)open(path, flags, mode);
    }

    /**
     * @brief 析构函数 - 自动关闭文件描述符
     */
    ~FileDescriptor() noexcept
    {
        close();
    }

    // 禁用拷贝
    FileDescriptor(const FileDescriptor&) = delete;
    FileDescriptor& operator=(const FileDescriptor&) = delete;

    // 启用移动
    FileDescriptor(FileDescriptor&& other) noexcept
        : m_fd(other.m_fd)
        , m_last_error(std::move(other.m_last_error))
    {
        other.m_fd = -1;
    }

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
     * @brief 打开文件
     * @param path 文件路径
     * @param flags 打开标志
     * @return 成功返回 void，失败返回 IOError(kOpenFailed)
     */
    std::expected<void, galay::kernel::IOError> open(const char* path, int flags)
    {
        close();
        m_fd = ::open(path, flags);
        if (m_fd < 0) {
            m_last_error = galay::kernel::IOError(galay::kernel::kOpenFailed, errno);
            return std::unexpected(*m_last_error);
        }
        m_last_error.reset();
        return {};
    }

    /**
     * @brief 打开文件（带权限）
     * @param path 文件路径
     * @param flags 打开标志
     * @param mode 文件权限
     * @return 成功返回 void，失败返回 IOError(kOpenFailed)
     */
    std::expected<void, galay::kernel::IOError> open(const char* path, int flags, mode_t mode)
    {
        close();
        m_fd = ::open(path, flags, mode);
        if (m_fd < 0) {
            m_last_error = galay::kernel::IOError(galay::kernel::kOpenFailed, errno);
            return std::unexpected(*m_last_error);
        }
        m_last_error.reset();
        return {};
    }

    /**
     * @brief 关闭文件描述符
     */
    void close() noexcept
    {
        if (m_fd >= 0) {
            ::close(m_fd);
            m_fd = -1;
        }
    }

    /**
     * @brief 获取文件描述符
     * @return 文件描述符
     */
    int get() const noexcept
    {
        return m_fd;
    }

    /**
     * @brief 检查文件描述符是否有效
     * @return 是否有效
     */
    bool valid() const noexcept
    {
        return m_fd >= 0;
    }

    /**
     * @brief 获取最近一次打开失败的错误
     * @return 若最近一次 open 失败则返回 IOError，否则返回 std::nullopt
     */
    const std::optional<galay::kernel::IOError>& lastError() const noexcept
    {
        return m_last_error;
    }

    /**
     * @brief 释放文件描述符所有权（不关闭）
     * @return 文件描述符
     */
    int release() noexcept
    {
        int fd = m_fd;
        m_fd = -1;
        return fd;
    }

    /**
     * @brief 交换两个文件描述符
     * @param other 另一个文件描述符对象
     */
    void swap(FileDescriptor& other) noexcept
    {
        std::swap(m_fd, other.m_fd);
    }

    /**
     * @brief bool 转换操作符
     */
    explicit operator bool() const noexcept
    {
        return valid();
    }

    /**
     * @brief 获取文件描述符（用于函数调用）
     */
    int operator*() const noexcept
    {
        return m_fd;
    }

private:
    int m_fd;
    std::optional<galay::kernel::IOError> m_last_error;
};

} // namespace galay::http

#endif // GALAY_FILE_DESCRIPTOR_H
