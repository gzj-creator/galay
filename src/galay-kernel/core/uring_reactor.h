/**
 * @file uring_reactor.h
 * @brief 基于 Linux io_uring 的 IO reactor
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 使用 Linux io_uring 实现高吞吐异步 IO 的 BackendReactor 接口。
 * 支持 multishot accept/recv（配合 provided buffer ring）、
 * send_zc（用于大负载零拷贝发送）和 eventfd 跨线程唤醒。
 */

#ifndef GALAY_KERNEL_IOURING_REACTOR_H
#define GALAY_KERNEL_IOURING_REACTOR_H

#include "backend_reactor.h"
#include "io_scheduler.hpp"
#include "wake_coordinator.h"

#ifdef USE_IOURING

#include <liburing.h>

// liburing/io_uring.h 定义了 BLOCK_SIZE 宏，与 concurrentqueue 冲突
#ifdef BLOCK_SIZE
#undef BLOCK_SIZE
#endif

#include <atomic>
#include <cstdint>
#include <memory>

namespace galay::kernel {

/**
 * @brief io_uring 后端 reactor
 * @details 负责 Linux 上 io_uring 请求的提交、完成轮询和 eventfd 唤醒。
 */
class IOUringReactor : public BackendReactor
{
public:
    IOUringReactor(int queue_depth, std::atomic<uint64_t>& last_error_code);  ///< 构造 io_uring reactor，并绑定错误输出槽位
    ~IOUringReactor() override;  ///< 释放 io_uring ring 和唤醒 fd 资源

    IOUringReactor(const IOUringReactor&) = delete;
    IOUringReactor& operator=(const IOUringReactor&) = delete;

    void notify() override;  ///< 从其他线程唤醒阻塞中的 io_uring wait
    int wakeReadFdForTest() const override;  ///< 返回测试可见的 eventfd 读端句柄

    int addAccept(IOController* controller);  ///< 注册 accept 请求；1=立即完成，0=已提交，<0=错误
    int addConnect(IOController* controller);  ///< 注册 connect 请求；1=立即完成，0=已提交，<0=错误
    int addRecv(IOController* controller);  ///< 注册 recv 请求；1=立即完成，0=已提交，<0=错误
    int addSend(IOController* controller);  ///< 注册 send 请求；1=立即完成，0=已提交，<0=错误
    int addReadv(IOController* controller);  ///< 注册 readv 请求；1=立即完成，0=已提交，<0=错误
    int addWritev(IOController* controller);  ///< 注册 writev 请求；1=立即完成，0=已提交，<0=错误
    int addClose(IOController* controller);  ///< 注册 close 请求；0=成功，<0=错误
    int addFileRead(IOController* controller);  ///< 注册文件读取请求；1=立即完成，0=已提交，<0=错误
    int addFileWrite(IOController* controller);  ///< 注册文件写入请求；1=立即完成，0=已提交，<0=错误
    int addRecvFrom(IOController* controller);  ///< 注册 recvfrom 请求；1=立即完成，0=已提交，<0=错误
    int addSendTo(IOController* controller);  ///< 注册 sendto 请求；1=立即完成，0=已提交，<0=错误
    int addFileWatch(IOController* controller);  ///< 注册文件监控请求；1=立即完成，0=已提交，<0=错误
    int addSendFile(IOController* controller);  ///< 注册 sendfile 请求；1=立即完成，0=已提交，<0=错误
    int addSequence(IOController* controller);  ///< 注册组合式序列请求；0=已提交或已唤醒立即完成 owner，<0=错误
    int remove(IOController* controller);  ///< 使控制器关联的未完成请求失效或移除

    void poll(uint64_t timeout_ns, WakeCoordinator& wake_coordinator);  ///< 等待完成事件并通过 wake coordinator 分发唤醒

private:
    int submitMultishotAccept(IOController* controller);  ///< 为 listener 提交持久 multishot accept SQE
    int submitMultishotRecv(IOController* controller);  ///< 为 socket 提交持久 multishot recv SQE
    bool shouldUseSendZc(size_t length) const noexcept;  ///< 当前 send 请求是否应走 send_zc 路径
    void prepareSendSqe(struct io_uring_sqe* sqe,
                        SqeRequestToken* token,
                        int fd,
                        const void* buffer,
                        size_t length,
                        int flags);  ///< 按能力/长度门控填充 send 或 send_zc SQE
    int submitSequenceSqe(IOController::Index slot,
                          IOEventType type,
                          IOContextBase* ctx,
                          IOController* controller,
                          SequenceAwaitableBase* owner);  ///< 为 sequence awaitable 提交指定槽位的 SQE
    void processAcceptCompletion(IOController* controller,
                                 AcceptAwaitable* awaitable,
                                 struct io_uring_cqe* cqe);  ///< 处理 multishot accept CQE 并交付/缓存 accepted fd
    void processRecvCompletion(IOController* controller,
                               RecvAwaitable* awaitable,
                               struct io_uring_cqe* cqe);  ///< 处理 multishot recv CQE 并交付/缓存 ready recv 数据
    void processCompletion(struct io_uring_cqe* cqe);  ///< 消费单个 CQE 并唤醒对应 awaitable
    void ensureWakeReadArmed();  ///< 确保 eventfd 的唤醒读请求已提交到 ring

    static constexpr uint16_t kRecvBufferGroup = 0;  ///< provided buffer ring 使用的固定 buffer group id
    static constexpr uint16_t kRecvBufferCount = 256;  ///< provided buffer ring 中预留的 buffer 数量
    static constexpr size_t kRecvBufferSize = 8192;  ///< 单个 provided buffer 的容量
    static constexpr size_t kSendZcThreshold = 4096;  ///< 大于等于该阈值的 send 请求优先尝试 send_zc

    struct io_uring m_ring {};  ///< io_uring ring 实例
    int m_queue_depth = 0;  ///< ring 队列深度
    int m_event_fd = -1;  ///< 跨线程唤醒用 eventfd
    uint64_t m_eventfd_buf = 0;  ///< eventfd 读缓冲
    bool m_wake_read_armed = false;  ///< eventfd 读请求是否已挂到 ring
    bool m_send_zc_supported = false;  ///< 当前内核/liburing 是否支持 IORING_OP_SEND_ZC
    std::shared_ptr<void> m_recv_buffer_pool;  ///< recv provided buffer ring 的共享所有权
    std::atomic<uint64_t>& m_last_error_code;  ///< 最近一次后端错误编码输出槽位
};

}  // namespace galay::kernel

#endif  // USE_IOURING

#endif  // GALAY_KERNEL_IOURING_REACTOR_H
