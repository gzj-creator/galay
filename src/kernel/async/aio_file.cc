/**
 * @file aio_file.cc
 * @brief 基于 Linux AIO (libaio) 的异步文件操作实现
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 包含 AioFile 和 AioCommitAwaitable 的具体实现。
 * 管理 libaio 上下文生命周期、基于 eventfd 的完成通知，
 * 以及通过 posix_memalign 进行对齐缓冲区分配。
 */

#include "aio_file.h"

#ifdef USE_EPOLL

#include "kernel/kernel/epoll_scheduler.h"
#include <unistd.h>
#include <sys/stat.h>
#include <sys/eventfd.h>
#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace galay::async
{

using namespace galay::kernel;

// ============================================================================
// AioCommitAwaitable 实现
// ============================================================================

/**
 * @brief 构造批量 AIO 提交等待体
 *
 * @param controller 用于事件注册的 IO 控制器
 * @param aio_ctx libaio 上下文句柄
 * @param event_fd 用于完成通知的 eventfd
 * @param pending_ptrs 所有权转移的待提交 iocb 指针集合
 * @param pending_count 实际提交的操作数
 */
AioCommitAwaitable::AioCommitAwaitable(IOController* controller,
                                       io_context_t aio_ctx, int event_fd,
                                       std::vector<struct iocb*>&& pending_ptrs, size_t pending_count)
    : m_controller(controller)
    , m_aio_ctx(aio_ctx)
    , m_event_fd(event_fd)
    , m_pending_ptrs(std::move(pending_ptrs))
    , m_pending_count(pending_count)
{
}

/**
 * @brief 将聚合提交结果返回给等待的协程
 * @return 成功时返回每个操作的结果向量，失败时返回 IOError
 */
std::expected<std::vector<ssize_t>, IOError> AioCommitAwaitable::await_resume()
{
    return std::move(m_result);
}

// ============================================================================
// AioFile 实现
// ============================================================================

/**
 * @brief 构造 AioFile 并初始化 libaio 上下文和 eventfd
 * @param max_events 最大并发 AIO 事件数
 */
AioFile::AioFile(int max_events)
    : m_handle(GHandle::invalid())
    , m_controller(GHandle::invalid())
    , m_aio_ctx(0)
    , m_event_fd(-1)
    , m_max_events(max_events)
{
    // 创建 AIO context
    if (io_setup(max_events, &m_aio_ctx) < 0) {
        m_aio_ctx = 0;
    }

    // 创建 eventfd 用于通知
    m_event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
}

/**
 * @brief 析构函数；关闭文件、销毁 AIO 上下文并关闭 eventfd
 */
AioFile::~AioFile()
{
    close();
    if (m_aio_ctx) {
        io_destroy(m_aio_ctx);
    }
    if (m_event_fd >= 0) {
        ::close(m_event_fd);
    }
}

/**
 * @brief 移动构造函数；转移所有资源，使源对象无效
 * @param other 被移动的对象
 */
AioFile::AioFile(AioFile&& other) noexcept
    : m_handle(other.m_handle)
    , m_controller(std::move(other.m_controller))
    , m_aio_ctx(other.m_aio_ctx)
    , m_event_fd(other.m_event_fd)
    , m_max_events(other.m_max_events)
    , m_pending_cbs(std::move(other.m_pending_cbs))
    , m_pending_ptrs(std::move(other.m_pending_ptrs))
{
    other.m_handle = GHandle::invalid();
    other.m_aio_ctx = 0;
    other.m_event_fd = -1;
}

/**
 * @brief 移动赋值运算符；先释放当前资源再转移
 * @param other 被移动的对象
 * @return 当前对象的引用
 */
AioFile& AioFile::operator=(AioFile&& other) noexcept
{
    if (this != &other) {
        close();
        if (m_aio_ctx) {
            io_destroy(m_aio_ctx);
        }
        if (m_event_fd >= 0) {
            ::close(m_event_fd);
        }

        m_handle = other.m_handle;
        m_controller = std::move(other.m_controller);
        m_aio_ctx = other.m_aio_ctx;
        m_event_fd = other.m_event_fd;
        m_max_events = other.m_max_events;
        m_pending_cbs = std::move(other.m_pending_cbs);
        m_pending_ptrs = std::move(other.m_pending_ptrs);

        other.m_handle = GHandle::invalid();
        other.m_aio_ctx = 0;
        other.m_event_fd = -1;
    }
    return *this;
}

/**
 * @brief 以从指定模式派生的 O_DIRECT 标志打开文件
 *
 * @param path 文件系统路径
 * @param mode 期望的打开模式（Read、Write、ReadWrite）
 * @param permissions 文件创建权限（默认 0644）
 * @return 成功返回 void，失败返回带 kOpenFailed 的 IOError
 */
std::expected<void, IOError> AioFile::open(const std::string& path, AioOpenMode mode, int permissions)
{
    int flags = static_cast<int>(mode);
    int fd = ::open(path.c_str(), flags, permissions);
    if (fd < 0) {
        return std::unexpected(IOError(kOpenFailed, errno));
    }
    m_handle.fd = fd;
    return {};
}

/**
 * @brief 准备并入队一个异步读 iocb
 * @param buffer 对齐的目标缓冲区
 * @param length 要读取的字节数
 * @param offset 起始文件偏移量
 */
void AioFile::preRead(char* buffer, size_t length, off_t offset)
{
    struct iocb cb;
    std::memset(&cb, 0, sizeof(cb));
    io_prep_pread(&cb, m_handle.fd, buffer, length, offset);
    io_set_eventfd(&cb, m_event_fd);

    m_pending_cbs.push_back(cb);
}

/**
 * @brief 准备并入队一个异步写 iocb
 * @param buffer 对齐的源缓冲区
 * @param length 要写入的字节数
 * @param offset 起始文件偏移量
 */
void AioFile::preWrite(const char* buffer, size_t length, off_t offset)
{
    struct iocb cb;
    std::memset(&cb, 0, sizeof(cb));
    io_prep_pwrite(&cb, m_handle.fd, const_cast<char*>(buffer), length, offset);
    io_set_eventfd(&cb, m_event_fd);

    m_pending_cbs.push_back(cb);
}

/**
 * @brief 将多个读操作批量入队，每个转发到 preRead
 * @param reads (buffer, length, offset) 元组向量
 */
void AioFile::preReadBatch(const std::vector<std::tuple<char*, size_t, off_t>>& reads)
{
    for (const auto& [buffer, length, offset] : reads) {
        preRead(buffer, length, offset);
    }
}

/**
 * @brief 将多个写操作批量入队，每个转发到 preWrite
 * @param writes (buffer, length, offset) 元组向量
 */
void AioFile::preWriteBatch(const std::vector<std::tuple<const char*, size_t, off_t>>& writes)
{
    for (const auto& [buffer, length, offset] : writes) {
        preWrite(buffer, length, offset);
    }
}

/**
 * @brief 从累积的 iocb 构建指针数组并返回提交等待体
 *
 * @details 待处理 iocb 指针的所有权转移到返回的等待体，
 * 以确保在协程挂起期间它们保持有效。
 *
 * @return AioCommitAwaitable，将提交并等待所有待处理的操作
 */
AioCommitAwaitable AioFile::commit()
{
    // 更新指针数组
    m_pending_ptrs.clear();
    m_pending_ptrs.reserve(m_pending_cbs.size());
    for (auto& cb : m_pending_cbs) {
        m_pending_ptrs.push_back(&cb);
    }

    size_t pending_count = m_pending_ptrs.size();

    // 移动 pending_ptrs 的所有权给 awaitable，避免生命周期问题
    std::vector<struct iocb*> ptrs_copy = m_pending_ptrs;
    return AioCommitAwaitable(&m_controller, m_aio_ctx, m_event_fd, std::move(ptrs_copy), pending_count);
}

/**
 * @brief 丢弃所有待处理但未提交的 iocb 和指针
 */
void AioFile::clear()
{
    m_pending_cbs.clear();
    m_pending_ptrs.clear();
}

/**
 * @brief 如果文件描述符当前处于打开状态则关闭它
 */
void AioFile::close()
{
    if (m_handle.fd >= 0) {
        ::close(m_handle.fd);
        m_handle = GHandle::invalid();
    }
}

/**
 * @brief 通过 fstat 查询当前文件大小
 * @return 成功时返回文件大小（字节），失败时返回带 kStatFailed 的 IOError
 */
std::expected<size_t, IOError> AioFile::size() const
{
    struct stat st;
    if (fstat(m_handle.fd, &st) < 0) {
        return std::unexpected(IOError(kStatFailed, errno));
    }
    return static_cast<size_t>(st.st_size);
}

/**
 * @brief 通过 fsync 将文件数据刷入稳定存储
 * @return 成功返回 void，失败返回带 kSyncFailed 的 IOError
 */
std::expected<void, IOError> AioFile::sync()
{
    if (fsync(m_handle.fd) < 0) {
        return std::unexpected(IOError(kSyncFailed, errno));
    }
    return {};
}

/**
 * @brief 使用 posix_memalign 分配 O_DIRECT 兼容的对齐缓冲区
 *
 * @param size 缓冲区大小（字节）
 * @param alignment 对齐边界（字节，默认 512）
 * @return 指向对齐缓冲区的指针，分配失败时返回 nullptr
 */
char* AioFile::allocAlignedBuffer(size_t size, size_t alignment)
{
    void* ptr = nullptr;
    if (posix_memalign(&ptr, alignment, size) != 0) {
        return nullptr;
    }
    return static_cast<char*>(ptr);
}

/**
 * @brief 释放先前由 allocAlignedBuffer 分配的缓冲区
 * @param buffer 指向待释放缓冲区的指针
 */
void AioFile::freeAlignedBuffer(char* buffer)
{
    free(buffer);
}

} // namespace galay::async

#endif // USE_EPOLL
