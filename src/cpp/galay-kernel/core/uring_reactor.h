/**
 * @file uring_reactor.h
 * @brief 基于 Linux io_uring 的 IO reactor
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 使用 Linux io_uring 满足高吞吐异步 IO 的 ReactorType concept。
 * 支持 multishot accept/recv/recvmsg（配合 provided buffer ring）、
 * send_zc（用于大负载零拷贝发送）和 eventfd 跨线程唤醒。
 */

#ifndef GALAY_KERNEL_IOURING_REACTOR_H
#define GALAY_KERNEL_IOURING_REACTOR_H

#include "backend_reactor.h"
#include "io_controller.hpp"
#include "wake_coordinator.h"
#include "operation_completion.hpp"

#ifdef USE_IOURING

#include <atomic>
#include <cstdint>
#include <expected>
#include <memory>
#include <vector>

namespace galay::kernel {

struct IOContextBase;

/**
 * @brief io_uring 后端 reactor
 * @details 负责 Linux 上 io_uring 请求的提交、完成轮询和 eventfd 唤醒。
 */
class IOUringReactor
{
public:
    IOUringReactor(int queue_depth, std::atomic<uint64_t>& last_error_code);  ///< 构造 io_uring reactor，并绑定错误输出槽位
    ~IOUringReactor();  ///< 释放 io_uring ring 和唤醒 fd 资源

    IOUringReactor(const IOUringReactor&) = delete;
    IOUringReactor& operator=(const IOUringReactor&) = delete;

    void notify();  ///< 从其他线程唤醒阻塞中的 io_uring wait
    GHandle get_handle() const;  ///< 返回测试可见的 eventfd 读端句柄
    std::expected<void, IOError> start();  ///< 显式初始化 eventfd、io_uring ring 和 recv buffer ring

    int add_accept(IOController* controller);  ///< 注册 accept 请求；1=立即完成，0=已提交，<0=错误
    /** @brief owner 发布单次 accept；false 表示同步完成且不发出恢复回调。 */
    bool submit_accept(AcceptAwaitable& awaitable, Waker&& waker);
    /** @brief owner timer 竞争同一 completion；只解绑 frame，不取消资源 multishot。 */
    void timeout_accept(AcceptAwaitable& awaitable);
    int add_connect(IOController* controller);  ///< 注册 connect 请求；1=立即完成，0=已提交，<0=错误
    int add_recv(IOController* controller);  ///< 注册 recv 请求；1=立即完成，0=已提交，<0=错误
    int add_send(IOController* controller);  ///< 注册 send 请求；1=立即完成，0=已提交，<0=错误
    int add_readv(IOController* controller);  ///< 注册 readv 请求；1=立即完成，0=已提交，<0=错误
    int add_writev(IOController* controller);  ///< 注册 writev 请求；1=立即完成，0=已提交，<0=错误
    int add_close(IOController* controller);  ///< 注册 close 请求；0=成功，<0=错误
    int add_file_read(IOController* controller);  ///< 注册文件读取请求；1=立即完成，0=已提交，<0=错误
    int add_file_write(IOController* controller);  ///< 注册文件写入请求；1=立即完成，0=已提交，<0=错误
    int add_recv_from(IOController* controller);  ///< 注册 recvfrom 请求；1=立即完成，0=已提交，<0=错误
    int add_send_to(IOController* controller);  ///< 注册 sendto 请求；1=立即完成，0=已提交，<0=错误
    int add_file_watch(IOController* controller);  ///< 注册文件监控请求；1=立即完成，0=已提交，<0=错误
    int add_send_file(IOController* controller);  ///< 注册 sendfile 请求；1=立即完成，0=已提交，<0=错误
    int add_sequence(IOController* controller);  ///< 注册组合式序列请求；0=已提交或已唤醒立即完成 owner，<0=错误
    int remove(IOController* controller);  ///< 使控制器关联的未完成请求失效或移除
    void stop_accepts();  ///< owner 停机入口：完成已登记 accept 的逻辑等待；物理 CQE drain 仍由后续门禁负责

    void poll(uint64_t timeout_ns, WakeCoordinator& wake_coordinator);  ///< 等待完成事件并通过 wake coordinator 分发唤醒

private:
    friend struct IOUringReactorTestAccess;  ///< 确定性 CQE 注入；不增加生产对象状态或回调。
    /** @brief 清除当前 frame 的 slot/timer 引用，再取唯一恢复权；持久请求不计入 frame refs。 */
    std::expected<ResumeCapability, OperationError> detach_accept(AcceptAwaitable& awaitable);
    int submit_multishot_accept(IOController* controller);  ///< 为 listener 提交持久 multishot accept SQE
    int submit_multishot_recv(IOController* controller);  ///< 为 socket 提交持久 multishot recv SQE
    int submit_multishot_recv_from(IOController* controller);  ///< 为 UDP socket 提交持久 multishot recvmsg SQE
    std::expected<void, IOError> initialize_recv_from_buffer_pool();  ///< 首次 UDP recvfrom 时惰性初始化 provided-buffer ring
    int add_recv_from_one_shot(IOController* controller,
                           RecvFromAwaitable* awaitable);  ///< 能力不足时提交兼容 one-shot recvmsg SQE
    bool should_use_send_zc(size_t length) const noexcept;  ///< 当前 send 请求是否应走 send_zc 路径
    void prepare_send_sqe(struct io_uring_sqe* sqe,
                        SqeRequestHandle* handle,
                        int fd,
                        const void* buffer,
                        size_t length,
                        int flags);  ///< 按能力/长度门控填充 send 或 send_zc SQE
    int submit_sequence_sqe(IOController::Index slot,
                          IOEventType type,
                          IOContextBase* ctx,
                          IOController* controller,
                          SequenceAwaitableBase* owner);  ///< 为 sequence awaitable 提交指定槽位的 SQE
    void process_accept_completion(IOController* controller,
                                 AcceptAwaitable* awaitable,
                                 struct io_uring_cqe* cqe);  ///< 处理 multishot accept CQE 并交付/缓存 accepted fd
    void process_recv_completion(IOController* controller,
                               RecvAwaitable* awaitable,
                               struct io_uring_cqe* cqe);  ///< 处理 multishot recv CQE 并交付/缓存 ready recv 数据
    void process_recv_from_completion(IOController* controller,
                                   RecvFromAwaitable* awaitable,
                                   SqeRequestHandle* handle,
                                   struct io_uring_cqe* cqe);  ///< 解析 multishot recvmsg CQE 并按数据报交付 payload/源地址
    void process_completion(struct io_uring_cqe* cqe);  ///< 消费单个 CQE 并唤醒对应 awaitable
    void ensure_wake_read_armed();  ///< 确保 eventfd 的唤醒读请求已提交到 ring

    static constexpr uint16_t kRecvBufferGroup = 0;  ///< provided buffer ring 使用的固定 buffer group id
    static constexpr uint16_t kRecvBufferCount = 256;  ///< provided buffer ring 中预留的 buffer 数量
    static constexpr size_t kRecvBufferSize = 8192;  ///< 单个 provided buffer 的容量
    static constexpr uint16_t kRecvFromBufferGroup = 1;  ///< UDP recvmsg 使用的独立 buffer group id
    static constexpr uint16_t kRecvFromBufferCount = 256;  ///< UDP provided buffer ring 的 buffer 数量
    static constexpr size_t kRecvFromBufferSize =
        65536 + sizeof(uint32_t) * 4 + sizeof(sockaddr_storage);  ///< 覆盖最大 UDP payload 与 recvmsg 元数据
    static constexpr size_t kSendZcThreshold = 4096;  ///< 大于等于该阈值的 send 请求优先尝试 send_zc

    struct io_uring m_ring {};  ///< io_uring ring 实例
    int m_queue_depth = 0;  ///< ring 队列深度
    int m_event_fd = -1;  ///< 跨线程唤醒用 eventfd
    uint64_t m_eventfd_buf = 0;  ///< eventfd 读缓冲
    bool m_ring_initialized = false;  ///< io_uring ring 是否已经初始化
    bool m_wake_read_armed = false;  ///< eventfd 读请求是否已挂到 ring
    bool m_send_zc_supported = false;  ///< 当前内核/liburing 是否支持 IORING_OP_SEND_ZC
    bool m_recvmsg_multishot_supported = false;  ///< 内核>=6.0、liburing 与 RECVMSG opcode 均支持 multishot
    bool m_recvmsg_multishot_confirmed = false;  ///< 是否已收到成功 CQE，避免把后续 EINVAL 误判为能力缺失
    std::shared_ptr<void> m_recv_buffer_pool;  ///< recv provided buffer ring 的共享所有权
    std::shared_ptr<void> m_recvfrom_buffer_pool;  ///< UDP recvmsg provided buffer ring 的共享所有权
    struct AcceptRegistration {
        std::shared_ptr<SqeHandleArena> arena;  ///< 保持 SQE state 地址到 late CQE 处理完成
        SqeState* state = nullptr;  ///< arena 内稳定 state；owner 原子解析当前 controller
        SqeRequestHandle* handle = nullptr;  ///< 当前持久 accept 请求；停机后 ring teardown 才能回收 self-reference
    };
    std::unordered_map<SqeState*, AcceptRegistration> m_accept_registrations;  ///< PERF: O(1) 查找的 listener accept resource registrations
    std::atomic<uint64_t>& m_last_error_code;  ///< 最近一次后端错误编码输出槽位
    uint32_t m_next_accept_generation = 1; ///< 单次 operation key；不等同于 persistent request generation。
    bool m_accept_stopping = false; ///< owner stop 后拒绝发布新的 frame 引用。
};

static_assert(ReactorType<IOUringReactor>);

}  // namespace galay::kernel

#endif  // USE_IOURING

#endif  // GALAY_KERNEL_IOURING_REACTOR_H
