/**
 * @file async_file.cc
 * @brief 协程友好的异步文件操作实现（kqueue/io_uring）
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 包含 AsyncFile 的具体实现，包括 open、read、write、close、size 和 sync。
 * 将实际的异步行为委托给 IO 调度器的可等待类型。
 */

#include "kernel/common/defn.hpp"
#include "async_file.h"

#if defined(USE_KQUEUE) || defined(USE_IOURING)

#include <unistd.h>
#include <sys/stat.h>
#include <cerrno>

namespace galay::async
{

using namespace galay::kernel;

/**
 * @brief 默认构造函数；以无效句柄初始化 IO 控制器
 */
AsyncFile::AsyncFile()
    : m_controller(GHandle::invalid())
{
}

/**
 * @brief 析构函数；不会自动关闭文件
 */
AsyncFile::~AsyncFile()
{
}

/**
 * @brief 移动构造函数；转移 IO 控制器
 * @param other 被移动的对象
 */
AsyncFile::AsyncFile(AsyncFile&& other) noexcept
    : m_controller(std::move(other.m_controller))
{
}

/**
 * @brief 移动赋值运算符；关闭当前句柄后再转移
 * @param other 被移动的对象
 * @return 当前对象的引用
 */
AsyncFile& AsyncFile::operator=(AsyncFile&& other) noexcept
{
    if (this != &other) {
        if (m_controller.m_handle != GHandle::invalid()) {
            ::close(m_controller.m_handle.fd);
        }
        m_controller = std::move(other.m_controller);
    }
    return *this;
}

/**
 * @brief 以指定模式和给定路径打开文件
 *
 * @param path 文件系统路径
 * @param mode 从 FileOpenMode 派生的打开模式
 * @param permissions 文件创建权限
 * @return 成功返回 void，失败返回带 kOpenFailed 的 IOError
 */
std::expected<void, IOError> AsyncFile::open(const std::string& path, FileOpenMode mode, int permissions)
{
    int flags = static_cast<int>(mode);
    int fd = ::open(path.c_str(), flags, permissions);
    if (fd < 0) {
        return std::unexpected(IOError(kOpenFailed, errno));
    }
    m_controller.m_handle.fd = fd;
    return {};
}

/**
 * @brief 创建用于异步文件读取的 FileReadAwaitable
 * @param buffer 读取数据的目标缓冲区
 * @param length 要读取的字节数
 * @param offset 起始文件偏移量
 * @return 绑定到该文件 IO 控制器的 FileReadAwaitable
 */
FileReadAwaitable AsyncFile::read(char* buffer, size_t length, off_t offset)
{
    return FileReadAwaitable(&m_controller, buffer, length, offset);
}

/**
 * @brief 创建用于异步文件写入的 FileWriteAwaitable
 * @param buffer 源数据缓冲区
 * @param length 要写入的字节数
 * @param offset 起始文件偏移量
 * @return 绑定到该文件 IO 控制器的 FileWriteAwaitable
 */
FileWriteAwaitable AsyncFile::write(const char* buffer, size_t length, off_t offset)
{
    return FileWriteAwaitable(&m_controller, buffer, length, offset);
}

/**
 * @brief 创建用于异步文件关闭的 CloseAwaitable
 * @return 绑定到该文件 IO 控制器的 CloseAwaitable
 */
CloseAwaitable AsyncFile::close()
{
    return CloseAwaitable(&m_controller);
}

/**
 * @brief 通过 fstat 查询当前文件大小
 * @return 成功时返回文件大小（字节），失败时返回带 kStatFailed 的 IOError
 */
std::expected<size_t, IOError> AsyncFile::size() const
{
    struct stat st;
    if (fstat(m_controller.m_handle.fd, &st) < 0) {
        return std::unexpected(IOError(kStatFailed, errno));
    }
    return static_cast<size_t>(st.st_size);
}

/**
 * @brief 通过 fsync 将文件数据刷入稳定存储
 * @return 成功返回 void，失败返回带 kSyncFailed 的 IOError
 */
std::expected<void, IOError> AsyncFile::sync()
{
    if (fsync(m_controller.m_handle.fd) < 0) {
        return std::unexpected(IOError(kSyncFailed, errno));
    }
    return {};
}

} // namespace galay::async

#endif // USE_KQUEUE || USE_IOURING
