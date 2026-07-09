/**
 * @file aio_file.h
 * @brief 基于 Linux AIO (libaio) 的异步文件操作
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 通过 Linux libaio 接口和 O_DIRECT 提供协程友好的异步文件 I/O。
 * 所有缓冲区必须正确对齐。操作通过 preRead/preWrite 累积，
 * 并通过 commit() 批量提交，commit() 返回一个可等待对象，会挂起调用协程，
 * 直到内核完成所有已提交的 I/O 请求。
 *
 * 仅在使用 USE_EPOLL 编译时可用。
 */

#ifndef GALAY_AIO_FILE_H
#define GALAY_AIO_FILE_H

#ifdef USE_EPOLL

#include "../core/io_scheduler.hpp"
#include "../core/waker.h"
#include "../common/error.h"
#include <expected>
#include <string>
#include <vector>
#include <fcntl.h>
#include <libaio.h>

namespace galay::kernel {
    class EpollScheduler;
}

namespace galay::async
{
/**
 * @brief AIO 文件打开模式
 * @details 基于 `O_DIRECT` 打开文件，要求调用方使用对齐缓冲区。
 */
/**
 * @brief AIO 操作的文件打开模式
 *
 * @details 所有模式均强制使用 O_DIRECT；调用方必须使用对齐缓冲区
 * （参见 allocAlignedBuffer）。
 */
enum class AioOpenMode : int {
    Read      = O_RDONLY | O_DIRECT,  ///< 只读直通模式
    Write     = O_WRONLY | O_CREAT | O_DIRECT,  ///< 只写直通模式，必要时创建文件
    ReadWrite = O_RDWR | O_CREAT | O_DIRECT,  ///< 读写直通模式，必要时创建文件
};

/**
 * @brief AioFile 前向声明
 */
class AioFile;

/**
 * @brief AIO 提交结果的可等待对象
 */
struct AioCommitAwaitable {
    /**
     * @brief 构造批量 AIO 提交等待体
     * @param controller 关联的 IO 控制器
     * @param aio_ctx libaio 上下文
     * @param event_fd 用于完成通知的 eventfd
     * @param pending_ptrs 本次待提交的 iocb 指针集合，所有权转移到等待体
     * @param pending_count 本次实际需要提交的操作数
     */
    AioCommitAwaitable(galay::kernel::IOController* controller,
                       io_context_t aio_ctx, int event_fd,
                       std::vector<struct iocb*>&& pending_ptrs, size_t pending_count);

    AioCommitAwaitable(AioCommitAwaitable&&) noexcept = default;
    AioCommitAwaitable& operator=(AioCommitAwaitable&&) noexcept = default;

private:
    AioCommitAwaitable(const AioCommitAwaitable&) = delete;
    AioCommitAwaitable& operator=(const AioCommitAwaitable&) = delete;

public:
    /**
     * @brief 检查是否可以避免挂起
     * @return 如果没有待提交的操作则返回 true
     */
    bool await_ready() { return m_pending_count == 0; }

    /**
     * @brief 提交所有待处理的 AIO 操作，并可选择挂起协程
     *
     * @details 通过 io_submit 提交 iocb，将 eventfd 注册到 IO 调度器以接收完成通知，
     * 并挂起调用者。
     *
     * @tparam Promise 协程 promise 类型
     * @param handle 当前协程句柄
     * @return 如果协程被挂起则返回 true，如果应立即恢复则返回 false
     */
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle);

    /**
     * @brief 获取批量 AIO 提交的结果
     * @return 成功时返回每个操作的返回值向量，失败时返回 IOError
     */
    std::expected<std::vector<ssize_t>, galay::kernel::IOError> await_resume();

    galay::kernel::IOController* m_controller;  ///< 关联的 IO 控制器
    io_context_t m_aio_ctx;  ///< libaio 上下文
    int m_event_fd;  ///< 完成通知用 eventfd
    std::vector<struct iocb*> m_pending_ptrs;  ///< 待提交 iocb 指针集合，等待体拥有其生命周期
    size_t m_pending_count;  ///< 本次需要提交的操作数
    galay::kernel::Waker m_waker;  ///< 完成后恢复提交协程的唤醒器
    std::vector<ssize_t> m_results;  ///< 每个 iocb 对应的原始返回值
    std::expected<std::vector<ssize_t>, galay::kernel::IOError> m_result;  ///< 聚合后的提交结果
};

/**
 * @brief Linux AIO 风格的异步文件操作
 *
 * 使用方式:
 *   AioFile file(scheduler);
 *   file.open("test.txt", AioOpenMode::Read);
 *
 *   // 准备多个操作
 *   file.preRead(buf1, len1, offset1);
 *   file.preRead(buf2, len2, offset2);
 *
 *   // 批量提交并等待完成
 *   auto results = co_await file.commit();
 */
/**
 * @brief Linux AIO 文件，用于批量异步 I/O 操作
 *
 * @details 通过 preRead/preWrite 累积读/写操作，然后通过 commit() 单次批量提交。
 * 使用 libaio 和 eventfd 通过 IO 调度器进行完成通知。
 *
 * @note 所有 I/O 操作均需要 O_DIRECT 对齐的缓冲区。
 * @note 仅在使用 USE_EPOLL 编译时可用。
 */
class AioFile
{
public:
    /**
     * @brief 构造 AioFile，指定 AIO 队列深度
     * @param max_events 最大并发 AIO 事件数（默认 64）
     */
    AioFile(int max_events = 64);

    /**
     * @brief 析构函数；关闭文件并销毁 AIO 上下文
     */
    ~AioFile();

    AioFile(const AioFile&) = delete;
    AioFile& operator=(const AioFile&) = delete;

    /**
     * @brief 移动构造函数；转移所有资源的所有权
     * @param other 被移动的对象；移动后处于无效状态
     */
    AioFile(AioFile&& other) noexcept;

    /**
     * @brief 移动赋值运算符；先释放当前资源再转移
     * @param other 被移动的对象
     * @return 当前对象的引用
     */
    AioFile& operator=(AioFile&& other) noexcept;

    /**
     * @brief 以指定模式打开文件（强制使用 O_DIRECT）
     *
     * @param path 目标文件的文件系统路径
     * @param mode 打开模式（Read、Write 或 ReadWrite）
     * @param permissions 文件创建权限（默认 0644）
     * @return 成功返回 void，失败返回 IOError
     *
     * @note 后续所有 I/O 操作均需要缓冲区对齐
     */
    std::expected<void, galay::kernel::IOError> open(
        const std::string& path,
        AioOpenMode mode,
        int permissions = 0644);

    /**
     * @brief 入队一个异步读操作
     * @param buffer 目标缓冲区；必须 O_DIRECT 对齐
     * @param length 要读取的字节数
     * @param offset 文件偏移量（字节）
     */
    void preRead(char* buffer, size_t length, off_t offset);

    /**
     * @brief 入队一个异步写操作
     * @param buffer 源缓冲区；必须 O_DIRECT 对齐
     * @param length 要写入的字节数
     * @param offset 文件偏移量（字节）
     */
    void preWrite(const char* buffer, size_t length, off_t offset);

    /**
     * @brief 批量入队多个读操作
     * @param reads (buffer, length, offset) 元组向量
     */
    void preReadBatch(const std::vector<std::tuple<char*, size_t, off_t>>& reads);

    /**
     * @brief 批量入队多个写操作
     * @param writes (buffer, length, offset) 元组向量
     */
    void preWriteBatch(const std::vector<std::tuple<const char*, size_t, off_t>>& writes);

    /**
     * @brief 提交所有累积的操作并返回一个可等待对象
     * @return AioCommitAwaitable，会挂起直到所有操作完成
     *
     * @note 待处理 iocb 的所有权转移到可等待对象
     */
    AioCommitAwaitable commit();

    /**
     * @brief 丢弃所有已累积但未提交的操作
     */
    void clear();

    /**
     * @brief 关闭文件描述符（不销毁 AIO 上下文）
     */
    void close();

    /**
     * @brief 获取底层文件句柄
     * @return 当前的 GHandle
     */
    GHandle handle() const { return m_handle; }

    /**
     * @brief 检查文件当前是否已打开且有效
     * @return 如果文件描述符有效则返回 true
     */
    bool isValid() const { return m_handle.fd >= 0; }

    /**
     * @brief 获取当前文件大小
     * @return 成功时返回文件大小（字节），失败时返回 IOError
     */
    std::expected<size_t, galay::kernel::IOError> size() const;

    /**
     * @brief 将文件数据刷入稳定存储
     * @return 成功返回 void，失败返回 IOError
     */
    std::expected<void, galay::kernel::IOError> sync();

    /**
     * @brief 分配 O_DIRECT 操作的对齐缓冲区
     * @param size 缓冲区大小（字节）
     * @param alignment 对齐边界（默认 512）
     * @return 指向对齐缓冲区的指针，失败时返回 nullptr
     */
    static char* allocAlignedBuffer(size_t size, size_t alignment = 512);

    /**
     * @brief 释放先前由 allocAlignedBuffer 分配的缓冲区
     * @param buffer 指向待释放缓冲区的指针
     */
    static void freeAlignedBuffer(char* buffer);

    /**
     * @brief 获取内部 IO 控制器（用于高级操作）
     * @return IOController 指针
     */
    galay::kernel::IOController* getController() { return &m_controller; }

private:
    GHandle m_handle;  ///< 当前文件句柄
    galay::kernel::IOController m_controller;  ///< 批量提交完成通知使用的 IO 控制器

    io_context_t m_aio_ctx;  ///< libaio 上下文
    int m_event_fd;  ///< 完成通知 eventfd
    int m_max_events;  ///< libaio 队列深度上限

    std::vector<struct iocb> m_pending_cbs;  ///< 待提交操作对象集合
    std::vector<struct iocb*> m_pending_ptrs;  ///< 指向 m_pending_cbs 的提交指针数组
};

} // namespace galay::async

template <typename Promise>
inline bool galay::async::AioCommitAwaitable::await_suspend(std::coroutine_handle<Promise> handle)
{
    m_waker = galay::kernel::Waker(handle);

    if (m_pending_count == 0) {
        m_result = std::vector<ssize_t>{};
        return false;
    }

    int ret = io_submit(m_aio_ctx, m_pending_count, m_pending_ptrs.data());
    if (ret < 0) {
        m_result = std::unexpected(
            galay::kernel::IOError(galay::kernel::kWriteFailed, static_cast<uint32_t>(-ret)));
        return false;
    }

    auto scheduler = m_waker.getScheduler();
    if (scheduler == nullptr || scheduler->type() != galay::kernel::kIOScheduler) {
        m_result = std::unexpected(
            galay::kernel::IOError(galay::kernel::kNotRunningOnIOScheduler, errno));
        return false;
    }
    auto io_scheduler = static_cast<galay::kernel::IOScheduler*>(scheduler);

    m_controller->m_handle.fd = m_event_fd;
    m_controller->fillAwaitable(FILEREAD, this);
    if (io_scheduler->addFileRead(m_controller) < 0) {
        m_result = std::unexpected(
            galay::kernel::IOError(galay::kernel::kReadFailed, errno));
        return false;
    }
    return true;
}

#endif // USE_EPOLL

#endif // GALAY_AIO_FILE_H
