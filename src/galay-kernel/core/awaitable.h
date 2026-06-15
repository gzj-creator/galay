/**
 * @file awaitable.h
 * @brief 异步IO可等待对象
 * @author galay-kernel
 * @version 3.3.0
 *
 * @details 三层继承结构：
 * - AwaitableBase: 基类（m_sqe_type, virtual ~）
 * - IOContextBase: 中间层（virtual handleComplete 纯虚函数）
 *   - XxxIOContext: IO参数 + result + handleComplete 实现
 *     - XxxAwaitable: m_controller + m_waker + await_* + TimeoutSupport
 * - CloseAwaitable: 直接继承 AwaitableBase（无IO参数，无handleComplete）
 * - SequenceAwaitable: 组合式序列 Awaitable，支持标准 IO 与本地解析步骤
 *
 * 所有 Awaitable 都支持超时：
 * @code
 * auto result = co_await socket.recv(buffer, size).timeout(5s);
 * @endcode
 *
 * @note 这些类型由TcpSocket内部创建，用户通常不需要直接使用
 */

#ifndef GALAY_KERNEL_AWAITABLE_H
#define GALAY_KERNEL_AWAITABLE_H

#include "galay-kernel/common/defn.hpp"
#include "galay-kernel/common/error.h"
#include "galay-kernel/common/host.hpp"
#include "timeout.hpp"
#include "watch_defs.hpp"
#include "waker.h"
#include <cerrno>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <span>
#include <array>
#include <vector>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <sys/socket.h>
#include <sys/uio.h>

#ifdef USE_EPOLL
#include <libaio.h>
#endif

#include "io_handlers.hpp"

#include "io_controller.hpp"

namespace galay::kernel
{

// ==================== 第一层：AwaitableBase ====================

/**
 * @brief 所有 awaitable 的公共基类
 * @details 在 io_uring 模式下保存当前 SQE 对应的事件类型，供完成回调核对。
 */
struct AwaitableBase {
#ifdef USE_IOURING
    IOEventType m_sqe_type = IOEventType::INVALID;  ///< 当前 SQE 对应的事件类型
#endif
    virtual ~AwaitableBase() = default;  ///< 虚析构，允许通过基类安全释放具体 awaitable
};

/**
 * @brief await 挂起时可提取的上下文信息
 * @details 供状态机或高级 builder 在首次挂起时获取父任务与调度器信息。
 */
struct AwaitContext {
    TaskRef task;  ///< 当前挂起任务的轻量引用
    Scheduler* scheduler = nullptr;  ///< 当前任务所属调度器
};

/**
 * @brief Sequence awaitable 对 IOController 读写槽位的占用范围
 */
enum class SequenceOwnerDomain : uint8_t {
    Read,       ///< 仅占用读槽位
    Write,      ///< 仅占用写槽位
    ReadWrite   ///< 同时占用读写槽位
};

// ==================== 第二层：IOContextBase ====================

/**
 * @brief IO 上下文抽象基类
 * @details 保存一次 IO 操作的参数与完成回调接口，供 reactor 在完成事件到达时回填结果。
 */
struct IOContextBase: public AwaitableBase {
#ifdef USE_IOURING
    virtual bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) = 0;  ///< 消费 CQE 并返回该操作是否已完成
#else
    virtual bool handleComplete(GHandle handle) = 0;  ///< 在传统后端上消费一次就绪事件并返回该操作是否已完成
#endif

    // SequenceAwaitable 调度时可由上下文动态指定下一次等待方向；
    // 返回 INVALID 表示沿用静态 task.type。
    virtual IOEventType type() const { return IOEventType::INVALID; }  ///< 返回动态事件方向；INVALID 表示沿用静态事件类型
};

struct AcceptIOContext;
struct RecvIOContext;
struct SendIOContext;
struct ReadvIOContext;
struct WritevIOContext;
struct ConnectIOContext;
struct FileReadIOContext;
struct FileWriteIOContext;
struct RecvFromIOContext;
struct SendToIOContext;
struct FileWatchIOContext;
struct SendFileIOContext;
struct SequenceAwaitableBase;

namespace detail {

int registerIOSchedulerEvent(Scheduler* scheduler,
                             IOEventType event,
                             IOController* controller) noexcept;
int registerIOSchedulerClose(Scheduler* scheduler,
                             IOController* controller) noexcept;

using SequenceInterestMask = uint8_t;

constexpr bool sequenceEventUsesSlot(IOEventType type,
                                     IOController::Index slot) noexcept;

constexpr SequenceInterestMask sequenceSlotMask(IOController::Index slot) noexcept {
    return static_cast<SequenceInterestMask>(1u << static_cast<uint8_t>(slot));
}

constexpr SequenceInterestMask sequenceInterestMask(IOEventType type) noexcept {
    SequenceInterestMask mask = 0;
    if (sequenceEventUsesSlot(type, IOController::READ)) {
        mask = static_cast<SequenceInterestMask>(mask | sequenceSlotMask(IOController::READ));
    }
    if (sequenceEventUsesSlot(type, IOController::WRITE)) {
        mask = static_cast<SequenceInterestMask>(mask | sequenceSlotMask(IOController::WRITE));
    }
    return mask;
}

SequenceInterestMask collectSequenceInterestMask(const IOController* controller) noexcept;  ///< 汇总 controller 上所有 sequence awaitable 的关注位
SequenceInterestMask syncSequenceInterestMask(IOController* controller) noexcept;  ///< 重新计算并写回 controller 的 sequence 关注位
void clearSequenceInterestMask(IOController* controller) noexcept;  ///< 清空 controller 的 sequence 关注位与 armed 位

inline uint32_t normalizeAwaitableErrno(int ret) noexcept {
    return (ret < 0 && ret != -1)
        ? static_cast<uint32_t>(-ret)
        : static_cast<uint32_t>(errno);
}

template <typename ResultT>
inline bool finalizeAwaitableAddResult(int ret,
                                       IOErrorCode io_error,
                                       std::expected<ResultT, IOError>& result) {
    if (ret == 1) {
        return false;
    }
    if (ret < 0) {
        result = std::unexpected(IOError(io_error, normalizeAwaitableErrno(ret)));
        return false;
    }
    return true;
}

template <IOEventType Event, typename AwaitableT>
inline auto resumeIOAwaitable(AwaitableT& awaitable) -> decltype(std::move(awaitable.m_result)) {
#ifdef USE_IOURING
    const bool owns_read =
        sequenceEventUsesSlot(Event, IOController::READ) &&
        awaitable.m_controller->m_awaitable[IOController::READ] == &awaitable;
    const bool owns_write =
        sequenceEventUsesSlot(Event, IOController::WRITE) &&
        awaitable.m_controller->m_awaitable[IOController::WRITE] == &awaitable;
    if (owns_read || owns_write) {
        awaitable.m_controller->removeAwaitable(Event);
    }
#else
    awaitable.m_controller->removeAwaitable(Event);
#endif
    return std::move(awaitable.m_result);
}

template <typename AwaitableT, IOEventType Event, IOErrorCode ErrorCode, typename Promise>
inline bool suspendRegisteredAwaitable(AwaitableT& awaitable, std::coroutine_handle<Promise> handle) {
    awaitable.m_waker = Waker(handle);
#ifdef USE_IOURING
    awaitable.m_sqe_type = Event;
#endif
    if ((sequenceEventUsesSlot(Event, IOController::READ) &&
         awaitable.m_controller->m_sequence_owner[IOController::READ] != nullptr) ||
        (sequenceEventUsesSlot(Event, IOController::WRITE) &&
         awaitable.m_controller->m_sequence_owner[IOController::WRITE] != nullptr)) {
        awaitable.m_result = std::unexpected(IOError(kNotReady, 0));
        return false;
    }
    awaitable.m_controller->fillAwaitable(Event, &awaitable);
    auto* scheduler = awaitable.m_waker.getScheduler();
    if (scheduler == nullptr || scheduler->type() != kIOScheduler) {
        awaitable.m_result = std::unexpected(IOError(kNotRunningOnIOScheduler, errno));
        return false;
    }
    const int ret = registerIOSchedulerEvent(scheduler, Event, awaitable.m_controller);
    return finalizeAwaitableAddResult(ret, ErrorCode, awaitable.m_result);
}

template <typename Promise>
inline bool suspendSequenceAwaitable(SequenceAwaitableBase& awaitable,
                                     std::coroutine_handle<Promise> handle);

template <typename Promise>
inline AwaitContext makeAwaitContext(std::coroutine_handle<Promise> handle) {
    TaskRef task = handle.promise().taskRefView();
    return AwaitContext{
        .task = task,
        .scheduler = task.belongScheduler(),
    };
}

template <typename TargetT>
inline void bindAwaitContextIfSupported(TargetT& target, const AwaitContext& ctx) {
    if constexpr (requires(TargetT& t, const AwaitContext& context) {
        t.onAwaitContext(context);
    }) {
        target.onAwaitContext(ctx);
    }
}

constexpr bool sequenceOwnerDomainUsesSlot(SequenceOwnerDomain domain,
                                           IOController::Index slot) noexcept {
    switch (domain) {
    case SequenceOwnerDomain::Read:
        return slot == IOController::READ;
    case SequenceOwnerDomain::Write:
        return slot == IOController::WRITE;
    case SequenceOwnerDomain::ReadWrite:
        return true;
    }
    return false;
}

constexpr bool sequenceEventUsesSlot(IOEventType type,
                                     IOController::Index slot) noexcept {
    const uint32_t t = static_cast<uint32_t>(type);
    if (slot == IOController::READ) {
        return (t & (ACCEPT | RECV | READV | RECVFROM | FILEREAD | FILEWATCH)) != 0;
    }
    if (slot == IOController::WRITE) {
        return (t & (CONNECT | SEND | WRITEV | SENDTO | FILEWRITE | SENDFILE)) != 0;
    }
    return false;
}

template <typename MachineT>
constexpr SequenceOwnerDomain resolveStateMachineOwnerDomain(const MachineT& machine) {
    if constexpr (requires {
        { MachineT::kSequenceOwnerDomain } -> std::convertible_to<SequenceOwnerDomain>;
    }) {
        return MachineT::kSequenceOwnerDomain;
    } else if constexpr (requires {
        { MachineT::sequence_owner_domain } -> std::convertible_to<SequenceOwnerDomain>;
    }) {
        return MachineT::sequence_owner_domain;
    } else if constexpr (requires(const MachineT& m) {
        { m.sequenceOwnerDomain() } -> std::convertible_to<SequenceOwnerDomain>;
    }) {
        return machine.sequenceOwnerDomain();
    } else {
        return SequenceOwnerDomain::ReadWrite;
    }
}

template <typename ContextT>
constexpr IOEventType customAwaitableDefaultEvent() {
    using T = std::remove_cvref_t<ContextT>;
    if constexpr (std::is_base_of_v<AcceptIOContext, T>) {
        return ACCEPT;
    } else if constexpr (std::is_base_of_v<RecvIOContext, T>) {
        return RECV;
    } else if constexpr (std::is_base_of_v<SendIOContext, T>) {
        return SEND;
    } else if constexpr (std::is_base_of_v<ReadvIOContext, T>) {
        return READV;
    } else if constexpr (std::is_base_of_v<WritevIOContext, T>) {
        return WRITEV;
    } else if constexpr (std::is_base_of_v<ConnectIOContext, T>) {
        return CONNECT;
    } else if constexpr (std::is_base_of_v<FileReadIOContext, T>) {
        return FILEREAD;
    } else if constexpr (std::is_base_of_v<FileWriteIOContext, T>) {
        return FILEWRITE;
    } else if constexpr (std::is_base_of_v<RecvFromIOContext, T>) {
        return RECVFROM;
    } else if constexpr (std::is_base_of_v<SendToIOContext, T>) {
        return SENDTO;
    } else if constexpr (std::is_base_of_v<FileWatchIOContext, T>) {
        return FILEWATCH;
    } else if constexpr (std::is_base_of_v<SendFileIOContext, T>) {
        return SENDFILE;
    } else {
        return IOEventType::INVALID;
    }
}

template <typename T>
struct is_expected : std::false_type {};

template <typename T, typename E>
struct is_expected<std::expected<T, E>> : std::true_type {};

template <typename ResultT>
constexpr bool is_expected_v = is_expected<std::remove_cvref_t<ResultT>>::value;

template <typename ResultT>
struct expected_traits;

template <typename T, typename E>
/**
 * @brief `std::expected<T, E>` 的 traits 特化
 * @details 供 sequence/state-machine 逻辑从 `expected` 结果类型中提取 value/error 类型。
 */
struct expected_traits<std::expected<T, E>> {
    using value_type = T;
    using error_type = E;
};

}  // namespace detail

// ==================== 第三层：IOContext + Awaitable ====================

// ---- Accept ----

/**
 * @brief accept 操作的上下文
 */
struct AcceptIOContext: public IOContextBase {
    AcceptIOContext(Host* host)
        : m_host(host) {}

#ifdef USE_IOURING
    bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override;  ///< 处理 io_uring accept 完成事件
#else
    bool handleComplete(GHandle handle) override;  ///< 处理传统后端 accept 就绪事件
#endif

    Host* m_host;  ///< 输出客户端地址；允许为 nullptr
    std::expected<GHandle, IOError> m_result;  ///< 接受连接的结果句柄
};

/**
 * @brief accept 的可等待对象
 * @details `co_await` 后返回新连接句柄，超时或失败时返回 `IOError`。
 */
struct AcceptAwaitable: public AcceptIOContext, public TimeoutSupport<AcceptAwaitable> {
    AcceptAwaitable(IOController* controller, Host* host)
        : AcceptIOContext(host), m_controller(controller) {}

    bool await_ready() { return false; }
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        return detail::suspendRegisteredAwaitable<AcceptAwaitable, ACCEPT, kAcceptFailed>(
            *this, handle);
    }
    std::expected<GHandle, IOError> await_resume();  ///< 返回 accept 结果；若失败则返回 IOError

    IOController* m_controller;  ///< 关联的 IO 控制器
    Waker m_waker;  ///< 恢复等待协程的唤醒器
};

// ---- Recv ----

/**
 * @brief recv 操作的上下文
 */
struct RecvIOContext: public IOContextBase {
    RecvIOContext(char* buffer, size_t length)
        : m_buffer(buffer), m_length(length) {}

#ifdef USE_IOURING
    bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override;  ///< 处理 io_uring recv 完成事件
#else
    bool handleComplete(GHandle handle) override;  ///< 处理传统后端 recv 就绪事件
#endif

    char* m_buffer;  ///< 接收缓冲区
    size_t m_length;  ///< 请求接收的最大字节数
    std::expected<size_t, IOError> m_result;  ///< 实际接收字节数或错误
};

/**
 * @brief recv 的可等待对象
 */
struct RecvAwaitable: public RecvIOContext, public TimeoutSupport<RecvAwaitable> {
    RecvAwaitable(IOController* controller, char* buffer, size_t length)
        : RecvIOContext(buffer, length), m_controller(controller) {}

    bool await_ready() { return false; }
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        return detail::suspendRegisteredAwaitable<RecvAwaitable, RECV, kRecvFailed>(
            *this, handle);
    }
    std::expected<size_t, IOError> await_resume();  ///< 返回实际接收字节数；0 可能表示 EOF

    IOController* m_controller;  ///< 关联的 IO 控制器
    Waker m_waker;  ///< 恢复等待协程的唤醒器
};

// ---- Send ----

/**
 * @brief send 操作的上下文
 */
struct SendIOContext: public IOContextBase {
    SendIOContext(const char* buffer, size_t length)
        : m_buffer(buffer), m_length(length) {}

#ifdef USE_IOURING
    bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override;  ///< 处理 io_uring send 完成事件
#else
    bool handleComplete(GHandle handle) override;  ///< 处理传统后端 send 就绪事件
#endif

    const char* m_buffer;  ///< 发送缓冲区
    size_t m_length;  ///< 请求发送的字节数
    std::expected<size_t, IOError> m_result;  ///< 实际发送字节数或错误
};

/**
 * @brief send 的可等待对象
 */
struct SendAwaitable: public SendIOContext, public TimeoutSupport<SendAwaitable> {
    SendAwaitable(IOController* controller, const char* buffer, size_t length)
        : SendIOContext(buffer, length), m_controller(controller) {}

    bool await_ready() { return false; }
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        return detail::suspendRegisteredAwaitable<SendAwaitable, SEND, kSendFailed>(
            *this, handle);
    }
    std::expected<size_t, IOError> await_resume();  ///< 返回实际发送字节数；可能小于请求长度

    IOController* m_controller;  ///< 关联的 IO 控制器
    Waker m_waker;  ///< 恢复等待协程的唤醒器
};

// ---- Readv ----

/**
 * @brief readv 操作的上下文
 */
struct ReadvIOContext: public IOContextBase {
    explicit ReadvIOContext(std::span<const struct iovec> iovecs)
        : m_iovecs(iovecs) {
#ifdef USE_IOURING
        initMsghdr();
#endif
    }

    template<size_t N>
    ReadvIOContext(std::array<struct iovec, N>& iovecs, size_t count)
        : m_iovecs(iovecs.data(), validateBorrowedCountOrAbort(count, N, "readv")) {
#ifdef USE_IOURING
        initMsghdr();
#endif
    }

    template<size_t N>
    ReadvIOContext(struct iovec (&iovecs)[N], size_t count)
        : m_iovecs(iovecs, validateBorrowedCountOrAbort(count, N, "readv")) {
#ifdef USE_IOURING
        initMsghdr();
#endif
    }

#ifdef USE_IOURING
    bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override;  ///< 处理 io_uring readv 完成事件
#else
    bool handleComplete(GHandle handle) override;  ///< 处理传统后端 readv 就绪事件
#endif

    static size_t validateBorrowedCountOrAbort(size_t count, size_t capacity, const char* op) {
        if (count <= capacity) {
            return count;
        }
        std::fprintf(stderr,
                     "invalid borrowed %s count: %zu > %zu\n",
                     op,
                     count,
                     capacity);
        std::abort();
    }

    std::span<const struct iovec> m_iovecs;  ///< 借用的 iovec 数组视图
    std::expected<size_t, IOError> m_result;  ///< 实际读取字节数或错误

#ifdef USE_IOURING
    void initMsghdr() {
        m_msg.msg_iov = const_cast<struct iovec*>(m_iovecs.data());
        m_msg.msg_iovlen = m_iovecs.size();
    }

    struct msghdr m_msg{};  ///< io_uring 使用的辅助 msghdr
#endif
};

/**
 * @brief readv 的可等待对象
 */
struct ReadvAwaitable: public ReadvIOContext, public TimeoutSupport<ReadvAwaitable> {
    ReadvAwaitable(IOController* controller, std::span<const struct iovec> iovecs)
        : ReadvIOContext(iovecs), m_controller(controller) {}

    template<size_t N>
    ReadvAwaitable(IOController* controller, std::array<struct iovec, N>& iovecs, size_t count)
        : ReadvIOContext(iovecs, count), m_controller(controller) {}

    template<size_t N>
    ReadvAwaitable(IOController* controller, struct iovec (&iovecs)[N], size_t count)
        : ReadvIOContext(iovecs, count), m_controller(controller) {}

    bool await_ready() { return false; }
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        return detail::suspendRegisteredAwaitable<ReadvAwaitable, READV, kRecvFailed>(
            *this, handle);
    }
    std::expected<size_t, IOError> await_resume();  ///< 返回实际读取字节数或错误

    IOController* m_controller;  ///< 关联的 IO 控制器
    Waker m_waker;  ///< 恢复等待协程的唤醒器
};

// ---- Writev ----

/**
 * @brief writev 操作的上下文
 */
struct WritevIOContext: public IOContextBase {
    explicit WritevIOContext(std::span<const struct iovec> iovecs)
        : m_iovecs(iovecs) {
#ifdef USE_IOURING
        initMsghdr();
#endif
    }

    template<size_t N>
    WritevIOContext(std::array<struct iovec, N>& iovecs, size_t count)
        : m_iovecs(iovecs.data(), validateBorrowedCountOrAbort(count, N, "writev")) {
#ifdef USE_IOURING
        initMsghdr();
#endif
    }

    template<size_t N>
    WritevIOContext(struct iovec (&iovecs)[N], size_t count)
        : m_iovecs(iovecs, validateBorrowedCountOrAbort(count, N, "writev")) {
#ifdef USE_IOURING
        initMsghdr();
#endif
    }

#ifdef USE_IOURING
    bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override;  ///< 处理 io_uring writev 完成事件
#else
    bool handleComplete(GHandle handle) override;  ///< 处理传统后端 writev 就绪事件
#endif

    static size_t validateBorrowedCountOrAbort(size_t count, size_t capacity, const char* op) {
        if (count <= capacity) {
            return count;
        }
        std::fprintf(stderr,
                     "invalid borrowed %s count: %zu > %zu\n",
                     op,
                     count,
                     capacity);
        std::abort();
    }

    std::span<const struct iovec> m_iovecs;  ///< 借用的 iovec 数组视图
    std::expected<size_t, IOError> m_result;  ///< 实际写入字节数或错误

#ifdef USE_IOURING
    void initMsghdr() {
        m_msg.msg_iov = const_cast<struct iovec*>(m_iovecs.data());
        m_msg.msg_iovlen = m_iovecs.size();
    }

    struct msghdr m_msg{};  ///< io_uring 使用的辅助 msghdr
#endif
};

/**
 * @brief writev 的可等待对象
 */
struct WritevAwaitable: public WritevIOContext, public TimeoutSupport<WritevAwaitable> {
    WritevAwaitable(IOController* controller, std::span<const struct iovec> iovecs)
        : WritevIOContext(iovecs), m_controller(controller) {}

    template<size_t N>
    WritevAwaitable(IOController* controller, std::array<struct iovec, N>& iovecs, size_t count)
        : WritevIOContext(iovecs, count), m_controller(controller) {}

    template<size_t N>
    WritevAwaitable(IOController* controller, struct iovec (&iovecs)[N], size_t count)
        : WritevIOContext(iovecs, count), m_controller(controller) {}

    bool await_ready() { return false; }
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        return detail::suspendRegisteredAwaitable<WritevAwaitable, WRITEV, kSendFailed>(
            *this, handle);
    }
    std::expected<size_t, IOError> await_resume();  ///< 返回实际写入字节数或错误

    IOController* m_controller;  ///< 关联的 IO 控制器
    Waker m_waker;  ///< 恢复等待协程的唤醒器
};

// ---- Connect ----

/**
 * @brief connect 操作的上下文
 */
struct ConnectIOContext: public IOContextBase {
    ConnectIOContext(const Host& host)
        : m_host(host) {}

#ifdef USE_IOURING
    bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override;  ///< 处理 io_uring connect 完成事件
#else
    bool handleComplete(GHandle handle) override;  ///< 处理传统后端 connect 就绪事件
#endif

    Host m_host;  ///< 目标地址
    std::expected<void, IOError> m_result;  ///< 连接结果
};

/**
 * @brief connect 的可等待对象
 */
struct ConnectAwaitable: public ConnectIOContext, public TimeoutSupport<ConnectAwaitable> {
    ConnectAwaitable(IOController* controller, const Host& host)
        : ConnectIOContext(host), m_controller(controller) {}

    bool await_ready() { return false; }
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        return detail::suspendRegisteredAwaitable<ConnectAwaitable, CONNECT, kConnectFailed>(
            *this, handle);
    }
    std::expected<void, IOError> await_resume();  ///< 返回连接结果；失败时返回 IOError

    IOController* m_controller;  ///< 关联的 IO 控制器
    Waker m_waker;  ///< 恢复等待协程的唤醒器
};

// ---- Close (直接继承 AwaitableBase，无 IOContext) ----

/**
 * @brief close 的可等待对象
 * @details 关闭请求会立即尝试向当前 IO scheduler 提交，恢复后返回关闭结果。
 */
struct CloseAwaitable: public AwaitableBase, public TimeoutSupport<CloseAwaitable> {
    CloseAwaitable(IOController* controller)
        : m_controller(controller) {}

    bool await_ready() { return false; }
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        m_waker = Waker(handle);
        auto scheduler = m_waker.getScheduler();
        if (scheduler == nullptr || scheduler->type() != kIOScheduler) {
            m_result = std::unexpected(IOError(kNotRunningOnIOScheduler, errno));
            return false;
        }
        int res = detail::registerIOSchedulerClose(scheduler, m_controller);
        if (res == 0) {
            m_result = {};
            return false;
        }
        m_result = std::unexpected(IOError(kDisconnectError, detail::normalizeAwaitableErrno(res)));
        return false;
    }
    std::expected<void, IOError> await_resume();  ///< 返回关闭结果；失败时返回 IOError

    IOController* m_controller;  ///< 关联的 IO 控制器
    Waker m_waker;  ///< 恢复等待协程的唤醒器
    std::expected<void, IOError> m_result;  ///< 关闭操作结果
};

// ---- RecvFrom ----

/**
 * @brief recvfrom 操作的上下文
 */
struct RecvFromIOContext: public IOContextBase {
    RecvFromIOContext(char* buffer, size_t length, Host* from)
        : m_buffer(buffer), m_length(length), m_from(from) {}

#ifdef USE_IOURING
    bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override;  ///< 处理 io_uring recvfrom 完成事件
#else
    bool handleComplete(GHandle handle) override;  ///< 处理传统后端 recvfrom 就绪事件
#endif

    char* m_buffer;  ///< 接收缓冲区
    size_t m_length;  ///< 请求接收的最大字节数
    Host* m_from;  ///< 输出对端地址；允许为 nullptr
    std::expected<size_t, IOError> m_result;  ///< 实际接收字节数或错误

#ifdef USE_IOURING
    struct msghdr m_msg;  ///< io_uring 使用的辅助 msghdr
    struct iovec m_iov;  ///< io_uring 使用的单段 iovec
    sockaddr_storage m_addr;  ///< io_uring 使用的临时地址缓冲
#endif
};

/**
 * @brief recvfrom 的可等待对象
 */
struct RecvFromAwaitable: public RecvFromIOContext, public TimeoutSupport<RecvFromAwaitable> {
    RecvFromAwaitable(IOController* controller, char* buffer, size_t length, Host* from)
        : RecvFromIOContext(buffer, length, from), m_controller(controller) {}

    bool await_ready() { return false; }
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        return detail::suspendRegisteredAwaitable<RecvFromAwaitable, RECVFROM, kRecvFailed>(
            *this, handle);
    }
    std::expected<size_t, IOError> await_resume();  ///< 返回实际接收字节数或错误

    IOController* m_controller;  ///< 关联的 IO 控制器
    Waker m_waker;  ///< 恢复等待协程的唤醒器
};

// ---- SendTo ----

/**
 * @brief sendto 操作的上下文
 */
struct SendToIOContext: public IOContextBase {
    SendToIOContext(const char* buffer, size_t length, const Host& to)
        : m_buffer(buffer), m_length(length), m_to(to) {}

#ifdef USE_IOURING
    bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override;  ///< 处理 io_uring sendto 完成事件
#else
    bool handleComplete(GHandle handle) override;  ///< 处理传统后端 sendto 就绪事件
#endif

    const char* m_buffer;  ///< 发送缓冲区
    size_t m_length;  ///< 请求发送的字节数
    Host m_to;  ///< 目标地址
    std::expected<size_t, IOError> m_result;  ///< 实际发送字节数或错误

#ifdef USE_IOURING
    struct msghdr m_msg;  ///< io_uring 使用的辅助 msghdr
    struct iovec m_iov;  ///< io_uring 使用的单段 iovec
#endif
};

/**
 * @brief sendto 的可等待对象
 */
struct SendToAwaitable: public SendToIOContext, public TimeoutSupport<SendToAwaitable> {
    SendToAwaitable(IOController* controller, const char* buffer, size_t length, const Host& to)
        : SendToIOContext(buffer, length, to), m_controller(controller) {}

    bool await_ready() { return false; }
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        return detail::suspendRegisteredAwaitable<SendToAwaitable, SENDTO, kSendFailed>(
            *this, handle);
    }
    std::expected<size_t, IOError> await_resume();  ///< 返回实际发送字节数或错误

    IOController* m_controller;  ///< 关联的 IO 控制器
    Waker m_waker;  ///< 恢复等待协程的唤醒器
};

// ---- FileRead ----

/**
 * @brief 文件读操作的上下文
 */
struct FileReadIOContext: public IOContextBase {
#ifdef USE_EPOLL
    FileReadIOContext(char* buffer, size_t length, off_t offset,
                      int event_fd, io_context_t aio_ctx, size_t expect_count = 1)
        : m_buffer(buffer), m_length(length), m_offset(offset),
          m_event_fd(event_fd), m_aio_ctx(aio_ctx), m_expect_count(expect_count) {}
#else
    FileReadIOContext(char* buffer, size_t length, off_t offset)
        : m_buffer(buffer), m_length(length), m_offset(offset) {}
#endif

#ifdef USE_IOURING
    bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override;  ///< 处理 io_uring 文件读完成事件
#else
    bool handleComplete(GHandle handle) override;  ///< 处理传统后端文件读完成事件
#endif

    char* m_buffer;  ///< 读取缓冲区
    size_t m_length;  ///< 请求读取的字节数
    off_t m_offset;  ///< 文件偏移
    std::expected<size_t, IOError> m_result;  ///< 实际读取字节数或错误

#ifdef USE_EPOLL
    int m_event_fd;  ///< epoll + libaio 模式下的 eventfd
    io_context_t m_aio_ctx;  ///< epoll + libaio 模式下的 AIO 上下文
    size_t m_expect_count;  ///< 期望完成的 AIO 操作数
    size_t m_finished_count{0};  ///< 已完成的 AIO 操作数
#endif
};

/**
 * @brief 文件读的可等待对象
 */
struct FileReadAwaitable: public FileReadIOContext, public TimeoutSupport<FileReadAwaitable> {
#ifdef USE_EPOLL
    FileReadAwaitable(IOController* controller,
                      char* buffer, size_t length, off_t offset,
                      int event_fd, io_context_t aio_ctx, size_t expect_count = 1)
        : FileReadIOContext(buffer, length, offset, event_fd, aio_ctx, expect_count),
          m_controller(controller) {}
#else
    FileReadAwaitable(IOController* controller,
                      char* buffer, size_t length, off_t offset)
        : FileReadIOContext(buffer, length, offset),
          m_controller(controller) {}
#endif

    bool await_ready() { return false; }
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        return detail::suspendRegisteredAwaitable<FileReadAwaitable, FILEREAD, kReadFailed>(
            *this, handle);
    }
    std::expected<size_t, IOError> await_resume();  ///< 返回实际读取字节数或错误

    IOController* m_controller;  ///< 关联的 IO 控制器
    Waker m_waker;  ///< 恢复等待协程的唤醒器
};

// ---- FileWrite ----

/**
 * @brief 文件写操作的上下文
 */
struct FileWriteIOContext: public IOContextBase {
#ifdef USE_EPOLL
    FileWriteIOContext(const char* buffer, size_t length, off_t offset,
                       int event_fd, io_context_t aio_ctx, size_t expect_count = 1)
        : m_buffer(buffer), m_length(length), m_offset(offset),
          m_event_fd(event_fd), m_aio_ctx(aio_ctx), m_expect_count(expect_count) {}
#else
    FileWriteIOContext(const char* buffer, size_t length, off_t offset)
        : m_buffer(buffer), m_length(length), m_offset(offset) {}
#endif

#ifdef USE_IOURING
    bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override;  ///< 处理 io_uring 文件写完成事件
#else
    bool handleComplete(GHandle handle) override;  ///< 处理传统后端文件写完成事件
#endif

    const char* m_buffer;  ///< 写入缓冲区
    size_t m_length;  ///< 请求写入的字节数
    off_t m_offset;  ///< 文件偏移
    std::expected<size_t, IOError> m_result;  ///< 实际写入字节数或错误

#ifdef USE_EPOLL
    int m_event_fd;  ///< epoll + libaio 模式下的 eventfd
    io_context_t m_aio_ctx;  ///< epoll + libaio 模式下的 AIO 上下文
    size_t m_expect_count;  ///< 期望完成的 AIO 操作数
    size_t m_finished_count{0};  ///< 已完成的 AIO 操作数
#endif
};

/**
 * @brief 文件写的可等待对象
 */
struct FileWriteAwaitable: public FileWriteIOContext, public TimeoutSupport<FileWriteAwaitable> {
#ifdef USE_EPOLL
    FileWriteAwaitable(IOController* controller,
                       const char* buffer, size_t length, off_t offset,
                       int event_fd, io_context_t aio_ctx, size_t expect_count = 1)
        : FileWriteIOContext(buffer, length, offset, event_fd, aio_ctx, expect_count),
          m_controller(controller) {}
#else
    FileWriteAwaitable(IOController* controller,
                       const char* buffer, size_t length, off_t offset)
        : FileWriteIOContext(buffer, length, offset),
          m_controller(controller) {}
#endif

    bool await_ready() { return false; }
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        return detail::suspendRegisteredAwaitable<FileWriteAwaitable, FILEWRITE, kWriteFailed>(
            *this, handle);
    }
    std::expected<size_t, IOError> await_resume();  ///< 返回实际写入字节数或错误

    IOController* m_controller;  ///< 关联的 IO 控制器
    Waker m_waker;  ///< 恢复等待协程的唤醒器
};

// ---- FileWatch ----

/**
 * @brief 文件监控操作的上下文
 */
struct FileWatchIOContext: public IOContextBase {
#ifdef USE_KQUEUE
    FileWatchIOContext(char* buffer, size_t buffer_size, FileWatchEvent events)
        : m_buffer(buffer), m_buffer_size(buffer_size), m_events(events) {}
#else
    FileWatchIOContext(char* buffer, size_t buffer_size)
        : m_buffer(buffer), m_buffer_size(buffer_size) {}
#endif

#ifdef USE_IOURING
    bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override;  ///< 处理 io_uring 文件监控完成事件
#else
    bool handleComplete(GHandle handle) override;  ///< 处理传统后端文件监控事件
#endif

    char* m_buffer;  ///< 监控事件输出缓冲区
    size_t m_buffer_size;  ///< 输出缓冲区容量
#ifdef USE_KQUEUE
    FileWatchEvent m_events;  ///< kqueue 模式下的监控事件掩码
#endif
    std::expected<FileWatchResult, IOError> m_result;  ///< 文件监控结果或错误
};

/**
 * @brief 文件监控的可等待对象
 */
struct FileWatchAwaitable: public FileWatchIOContext, public TimeoutSupport<FileWatchAwaitable> {
#ifdef USE_KQUEUE
    FileWatchAwaitable(IOController* controller,
                       char* buffer, size_t buffer_size,
                       FileWatchEvent events)
        : FileWatchIOContext(buffer, buffer_size, events),
          m_controller(controller) {}
#else
    FileWatchAwaitable(IOController* controller,
                       char* buffer, size_t buffer_size)
        : FileWatchIOContext(buffer, buffer_size),
          m_controller(controller) {}
#endif

    bool await_ready() { return false; }
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        return detail::suspendRegisteredAwaitable<FileWatchAwaitable, FILEWATCH, kReadFailed>(
            *this, handle);
    }
    std::expected<FileWatchResult, IOError> await_resume();  ///< 返回文件监控结果或错误

    IOController* m_controller;  ///< 关联的 IO 控制器
    Waker m_waker;  ///< 恢复等待协程的唤醒器
};

// ---- SendFile ----

/**
 * @brief sendfile 操作的上下文
 */
struct SendFileIOContext: public IOContextBase {
    SendFileIOContext(int file_fd, off_t offset, size_t count)
        : m_file_fd(file_fd), m_offset(offset), m_count(count) {}

#ifdef USE_IOURING
    bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override;  ///< 处理 io_uring sendfile 完成事件
#else
    bool handleComplete(GHandle handle) override;  ///< 处理传统后端 sendfile 就绪事件
#endif

    int m_file_fd;  ///< 源文件 fd
    off_t m_offset;  ///< 发送起始偏移
    size_t m_count;  ///< 请求发送的字节数
    std::expected<size_t, IOError> m_result;  ///< 实际发送字节数或错误
};

/**
 * @brief sendfile 的可等待对象
 */
struct SendFileAwaitable: public SendFileIOContext, public TimeoutSupport<SendFileAwaitable> {
    SendFileAwaitable(IOController* controller, int file_fd, off_t offset, size_t count)
        : SendFileIOContext(file_fd, offset, count), m_controller(controller) {}

    bool await_ready() { return false; }
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        return detail::suspendRegisteredAwaitable<SendFileAwaitable, SENDFILE, kSendFailed>(
            *this, handle);
    }
    std::expected<size_t, IOError> await_resume();  ///< 返回实际发送字节数或错误

    IOController* m_controller;  ///< 关联的 IO 控制器
    Waker m_waker;  ///< 恢复等待协程的唤醒器
};

/**
 * @brief Sequence awaitable 的推进结果
 */
enum class SequenceProgress {
    kNeedWait,    ///< 还需要等待新的 IO 事件
    kCompleted,   ///< 当前 sequence 已结束
};

/**
 * @brief parser 步骤的推进结果
 */
enum class ParseStatus {
    kNeedMore,    ///< 需要重新挂载接收步骤以获取更多输入
    kContinue,    ///< 继续执行后续步骤
    kCompleted,   ///< 当前 parser 步骤已完成
};

/**
 * @brief 状态机对外发出的动作信号
 */
enum class MachineSignal {
    kContinue,     ///< 继续内联推进状态机
    kWaitRead,     ///< 等待 recv
    kWaitReadv,    ///< 等待 readv
    kWaitWrite,    ///< 等待 send
    kWaitWritev,   ///< 等待 writev
    kWaitConnect,  ///< 等待 connect
    kComplete,     ///< 状态机已完成并产生结果
    kFail,         ///< 状态机失败
};

template <typename ResultT>
/**
 * @brief 状态机单步动作描述
 * @tparam ResultT 状态机结果类型
 */
struct MachineAction {
    MachineSignal signal = MachineSignal::kContinue;  ///< 当前动作类型
    char* read_buffer = nullptr;  ///< read/recv 目标缓冲区
    size_t read_length = 0;  ///< read/recv 请求长度
    const struct iovec* iovecs = nullptr;  ///< readv/writev 的 iovec 指针
    size_t iov_count = 0;  ///< iovec 数量
    const char* write_buffer = nullptr;  ///< send/write 源缓冲区
    size_t write_length = 0;  ///< send/write 请求长度
    Host connect_host{};  ///< connect 目标地址
    std::optional<ResultT> result;  ///< 成功结果
    std::optional<IOError> error;  ///< 失败结果

    static MachineAction continue_() {
        return MachineAction{};
    }

    static MachineAction waitRead(char* buffer, size_t length) {
        MachineAction action;
        action.signal = MachineSignal::kWaitRead;
        action.read_buffer = buffer;
        action.read_length = length;
        return action;
    }

    static MachineAction waitWrite(const char* buffer, size_t length) {
        MachineAction action;
        action.signal = MachineSignal::kWaitWrite;
        action.write_buffer = buffer;
        action.write_length = length;
        return action;
    }

    static MachineAction waitReadv(const struct iovec* iovecs, size_t count) {
        MachineAction action;
        action.signal = MachineSignal::kWaitReadv;
        action.iovecs = iovecs;
        action.iov_count = count;
        return action;
    }

    static MachineAction waitWritev(const struct iovec* iovecs, size_t count) {
        MachineAction action;
        action.signal = MachineSignal::kWaitWritev;
        action.iovecs = iovecs;
        action.iov_count = count;
        return action;
    }

    static MachineAction waitConnect(const Host& host) {
        MachineAction action;
        action.signal = MachineSignal::kWaitConnect;
        action.connect_host = host;
        return action;
    }

    static MachineAction complete(ResultT value) {
        MachineAction action;
        action.signal = MachineSignal::kComplete;
        action.result = std::move(value);
        return action;
    }

    static MachineAction fail(IOError io_error) {
        MachineAction action;
        action.signal = MachineSignal::kFail;
        action.error = std::move(io_error);
        return action;
    }
};

template <typename MachineT>
concept AwaitableStateMachine =
    requires(MachineT& machine, std::expected<size_t, IOError> io_result) {
        typename MachineT::result_type;
        { machine.advance() } -> std::same_as<MachineAction<typename MachineT::result_type>>;
        { machine.onRead(std::move(io_result)) } -> std::same_as<void>;
        { machine.onWrite(std::move(io_result)) } -> std::same_as<void>;
    };

/**
 * @brief 组合式 sequence awaitable 的抽象基类
 * @details 负责占用 IOController 的读写域、统一挂起/恢复流程以及错误传递。
 */
struct SequenceAwaitableBase: public AwaitableBase {
    /**
     * @brief sequence 队列中的单个任务条目
     */
    struct IOTask {
        IOEventType type;  ///< 默认事件类型
        void* task = nullptr;  ///< 具体任务对象指针
        IOContextBase* context = nullptr;  ///< 具体 IO 上下文指针
    };

    explicit SequenceAwaitableBase(IOController* controller,
                                   SequenceOwnerDomain requested_domain = SequenceOwnerDomain::ReadWrite)
        : m_controller(controller)
        , m_requested_domain(requested_domain)
        , m_registered_domain(requested_domain) {}

    virtual IOTask* front() = 0;  ///< 返回当前队首任务；为空时返回 nullptr
    virtual const IOTask* front() const = 0;  ///< 返回当前队首任务的只读视图；为空时返回 nullptr
    virtual void popFront() = 0;  ///< 弹出当前队首任务
    virtual bool empty() const = 0;  ///< 当前是否没有待执行的 sequence 条目

    IOEventType resolveTaskEventType(const IOTask& task) const {
        if (task.context == nullptr) {
            return task.type;
        }
        IOEventType desired = task.context->type();
        return desired == IOEventType::INVALID ? task.type : desired;
    }

    IOEventType activeEventType() const {
        const auto* task = front();
        return task == nullptr ? IOEventType::INVALID : resolveTaskEventType(*task);
    }

    bool waitsOn(IOController::Index slot) const {
        return detail::sequenceEventUsesSlot(activeEventType(), slot);
    }

    bool claimRequestedDomain() {
        if (m_controller == nullptr) {
            return false;
        }

        const bool need_read =
            detail::sequenceOwnerDomainUsesSlot(m_requested_domain, IOController::READ);
        const bool need_write =
            detail::sequenceOwnerDomainUsesSlot(m_requested_domain, IOController::WRITE);

        const auto can_claim = [this](IOController::Index slot) {
            return m_controller->m_awaitable[slot] == nullptr &&
                   (m_controller->m_sequence_owner[slot] == nullptr ||
                    m_controller->m_sequence_owner[slot] == this);
        };

        if ((need_read && !can_claim(IOController::READ)) ||
            (need_write && !can_claim(IOController::WRITE))) {
            return false;
        }

        if (need_read) {
            m_controller->m_sequence_owner[IOController::READ] = this;
        }
        if (need_write) {
            m_controller->m_sequence_owner[IOController::WRITE] = this;
        }
        m_controller->m_type |= SEQUENCE;
        m_registered_domain = m_requested_domain;
        m_registered = true;
        return true;
    }

    void releaseRegisteredDomain() {
        if (!m_registered || m_controller == nullptr) {
            m_registered = false;
            return;
        }

        if (detail::sequenceOwnerDomainUsesSlot(m_registered_domain, IOController::READ) &&
            m_controller->m_sequence_owner[IOController::READ] == this) {
            m_controller->m_sequence_owner[IOController::READ] = nullptr;
        }
        if (detail::sequenceOwnerDomainUsesSlot(m_registered_domain, IOController::WRITE) &&
            m_controller->m_sequence_owner[IOController::WRITE] == this) {
            m_controller->m_sequence_owner[IOController::WRITE] = nullptr;
        }
#ifdef USE_IOURING
        for (const auto slot : {IOController::READ, IOController::WRITE}) {
            if (m_controller->m_awaitable[slot] == this) {
                m_controller->m_awaitable[slot] = nullptr;
                m_controller->advanceSqeGeneration(slot);
            }
        }
#endif
        if (m_controller->m_sequence_owner[IOController::READ] == nullptr &&
            m_controller->m_sequence_owner[IOController::WRITE] == nullptr) {
            m_controller->m_type &= ~SEQUENCE;
            detail::clearSequenceInterestMask(m_controller);
        } else {
            (void)detail::syncSequenceInterestMask(m_controller);
        }
        m_registered = false;
    }

    void onCompleted() {
        releaseRegisteredDomain();
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        return detail::suspendSequenceAwaitable(*this, handle);
    }

#ifdef USE_IOURING
    virtual SequenceProgress prepareForSubmit() = 0;  ///< 为 io_uring 准备下一条待提交任务
    virtual SequenceProgress onActiveEvent(struct io_uring_cqe* cqe, GHandle handle) = 0;  ///< 处理 io_uring 当前活动任务的完成事件
#else
    virtual SequenceProgress prepareForSubmit(GHandle handle) = 0;  ///< 为传统后端准备下一条待执行任务
    virtual SequenceProgress onActiveEvent(GHandle handle) = 0;  ///< 处理传统后端当前活动任务的就绪事件
#endif

    std::optional<IOError> m_error;  ///< 当前 sequence 错误
    IOController* m_controller;  ///< 关联的 IO 控制器
    Waker m_waker;  ///< 恢复等待协程的唤醒器
    SequenceOwnerDomain m_requested_domain = SequenceOwnerDomain::ReadWrite;  ///< 期望占用的读写域
    SequenceOwnerDomain m_registered_domain = SequenceOwnerDomain::ReadWrite;  ///< 实际已登记的读写域
    bool m_registered = false;  ///< 当前是否已登记到 controller
};

namespace detail {

inline SequenceInterestMask collectSequenceInterestMask(const IOController* controller) noexcept {
    if (controller == nullptr) {
        return 0;
    }

    SequenceInterestMask mask = 0;
    const SequenceAwaitableBase* last_owner = nullptr;
    for (const auto slot : {IOController::READ, IOController::WRITE}) {
        const auto* owner = controller->m_sequence_owner[slot];
        if (owner == nullptr || owner == last_owner) {
            continue;
        }
        mask = static_cast<SequenceInterestMask>(mask | sequenceInterestMask(owner->activeEventType()));
        last_owner = owner;
    }
    return mask;
}

inline SequenceInterestMask syncSequenceInterestMask(IOController* controller) noexcept {
    if (controller == nullptr) {
        return 0;
    }

    controller->m_sequence_interest_mask = collectSequenceInterestMask(controller);
    return controller->m_sequence_interest_mask;
}

inline void clearSequenceInterestMask(IOController* controller) noexcept {
    if (controller == nullptr) {
        return;
    }

    controller->m_sequence_interest_mask = 0;
    controller->m_sequence_armed_mask = 0;
}

template <typename Promise>
inline bool suspendSequenceAwaitable(SequenceAwaitableBase& awaitable,
                                     std::coroutine_handle<Promise> handle) {
    awaitable.m_waker = Waker(handle);
    awaitable.m_registered = false;
    awaitable.m_error.reset();
#ifdef USE_IOURING
    awaitable.m_sqe_type = SEQUENCE;
#endif

    if (!awaitable.claimRequestedDomain()) {
        awaitable.m_error = IOError(kNotReady, 0);
        return false;
    }

#ifdef USE_IOURING
    if (awaitable.prepareForSubmit() == SequenceProgress::kCompleted) {
        return false;
    }
#else
    if (awaitable.prepareForSubmit(awaitable.m_controller->m_handle) == SequenceProgress::kCompleted) {
        return false;
    }
#endif

    (void)syncSequenceInterestMask(awaitable.m_controller);

    auto* scheduler = awaitable.m_waker.getScheduler();
    if (scheduler == nullptr || scheduler->type() != kIOScheduler) {
        awaitable.releaseRegisteredDomain();
        awaitable.m_error = IOError(kNotRunningOnIOScheduler, errno);
        return false;
    }
    const int ret = registerIOSchedulerEvent(scheduler, SEQUENCE, awaitable.m_controller);
    if (ret == 1) {
        return false;
    }
    if (ret < 0) {
        awaitable.releaseRegisteredDomain();
        awaitable.m_error = IOError(kNotReady, normalizeAwaitableErrno(ret));
        return false;
    }
    return true;
}

}  // namespace detail

template <typename ResultT, size_t InlineN = 4>
class SequenceAwaitable;

template <typename ResultT>
class ReadyAwaitable;

template <typename ResultT, size_t InlineN, typename FlowT>
class AwaitableBuilder;

/**
 * @brief Sequence awaitable 的辅助操作视图
 * @tparam ResultT sequence 结果类型
 * @tparam InlineN sequence 的内联任务容量
 */
template <typename ResultT, size_t InlineN = 4>
class SequenceOps {
public:
    explicit SequenceOps(SequenceAwaitable<ResultT, InlineN>& owner)
        : m_owner(owner) {}

    template <typename StepT>
    StepT& queue(StepT& step) {
        m_owner.queue(step);
        return step;
    }

    template <typename... StepTs>
    void queueMany(StepTs&... steps) {
        (queue(steps), ...);
    }

    void clear() {
        m_owner.clear();
    }

    template <typename ValueT>
    void complete(ValueT&& value) {
        m_owner.complete(std::forward<ValueT>(value));
    }

private:
    SequenceAwaitable<ResultT, InlineN>& m_owner;
};

/**
 * @brief 固定容量的组合式 sequence awaitable
 * @tparam ResultT sequence 结果类型
 * @tparam InlineN 可内联存放的任务条目数
 */
template <typename ResultT, size_t InlineN>
class SequenceAwaitable : public SequenceAwaitableBase {
public:
    /**
     * @brief sequence 中单个步骤的抽象基类
     * @details 支持纯本地步骤与真实 IO 步骤的统一排队和回调分发。
     */
    struct TaskBase {
        virtual ~TaskBase() = default;
        virtual IOContextBase* contextBase() = 0;  ///< 返回步骤关联的 IOContext；本地步骤可返回 nullptr
        virtual IOEventType defaultEventType() const = 0;  ///< 返回该步骤默认使用的 IO 事件类型
        virtual void beforeSubmit() {}  ///< 在真正提交给后端前执行的可选钩子
        virtual bool isLocal() const = 0;  ///< 当前步骤是否为纯本地步骤
#ifdef USE_IOURING
        virtual bool onEvent(SequenceAwaitable& owner, struct io_uring_cqe* cqe, GHandle handle) = 0;  ///< 处理 io_uring 事件并返回该步骤是否完成
#else
        virtual bool onReady(SequenceAwaitable& owner, GHandle handle) = 0;  ///< 在传统后端提交前尝试同步推进该步骤
        virtual bool onEvent(SequenceAwaitable& owner, GHandle handle) = 0;  ///< 处理传统后端就绪事件并返回该步骤是否完成
#endif
    };

    explicit SequenceAwaitable(IOController* controller,
                               SequenceOwnerDomain requested_domain = SequenceOwnerDomain::ReadWrite)
        : SequenceAwaitableBase(controller, requested_domain) {}

    bool await_ready() {
        return m_result_set;
    }

    auto await_resume() -> ResultT {
        onCompleted();
        if (m_result_set) {
            return std::move(*m_result);
        }
        if (m_error.has_value()) {
            if constexpr (detail::is_expected_v<ResultT>) {
                using ErrorT = typename detail::expected_traits<ResultT>::error_type;
                if constexpr (std::is_constructible_v<ErrorT, IOError>) {
                    return std::unexpected(ErrorT(*m_error));
                }
            }
        }
        if constexpr (detail::is_expected_v<ResultT>) {
            using ErrorT = typename detail::expected_traits<ResultT>::error_type;
            if constexpr (std::is_constructible_v<ErrorT, IOError>) {
                return std::unexpected(ErrorT(IOError(kNotReady, errno)));
            }
        }
        std::abort();
    }

    template <typename StepT>
    StepT& queue(StepT& step) {
        static_assert(std::is_base_of_v<TaskBase, std::remove_cvref_t<StepT>>,
                      "SequenceAwaitable::queue requires a Sequence task");
        emplaceTask(step.defaultEventType(), step.contextBase(), &step);
        return step;
    }

    TaskBase& queue(TaskBase& task) {
        emplaceTask(task.defaultEventType(), task.contextBase(), &task);
        return task;
    }

    template <typename StepT>
    StepT& queue(IOEventType type, StepT& step) {
        static_assert(std::is_base_of_v<TaskBase, std::remove_cvref_t<StepT>>,
                      "SequenceAwaitable::queue requires a Sequence task");
        emplaceTask(type, step.contextBase(), &step);
        return step;
    }

    TaskBase& queue(IOEventType type, TaskBase& task) {
        emplaceTask(type, task.contextBase(), &task);
        return task;
    }

    template <typename... StepTs>
    void queueMany(StepTs&... steps) {
        (queue(steps), ...);
    }

    void clear() {
        m_head = 0;
        m_size = 0;
    }

    template <typename ValueT>
    void complete(ValueT&& value) {
        m_result = std::forward<ValueT>(value);
        m_result_set = true;
        clear();
    }

    void fail(IOError error) {
        m_error = std::move(error);
        clear();
    }

    bool hasResultValue() const {
        return m_result_set && m_result.has_value();
    }

    bool hasFailure() const {
        return m_error.has_value();
    }

    std::optional<ResultT> takeResultValue() {
        m_result_set = false;
        auto result = std::move(m_result);
        m_result.reset();
        return result;
    }

    std::optional<IOError> takeFailure() {
        auto error = std::move(m_error);
        m_error.reset();
        return error;
    }

    void resetOutcomeForReuse() {
        m_result.reset();
        m_result_set = false;
        m_error.reset();
        clear();
    }

    SequenceOps<ResultT, InlineN> ops() {  ///< 返回 sequence 操作辅助视图
        return SequenceOps<ResultT, InlineN>(*this);
    }

    IOTask* front() override {
        if (m_size == 0) {
            return nullptr;
        }
        return &m_tasks[m_head];
    }

    const IOTask* front() const override {
        if (m_size == 0) {
            return nullptr;
        }
        return &m_tasks[m_head];
    }

    void popFront() override {
        if (m_size == 0) {
            return;
        }
        m_head = (m_head + 1) % InlineN;
        --m_size;
    }

    bool empty() const override {
        return m_size == 0;
    }

#ifdef USE_IOURING
    SequenceProgress prepareForSubmit() override {
        while (auto* entry = front()) {
            auto* task = static_cast<TaskBase*>(entry->task);
            if (!task) {
                popFront();
                continue;
            }
            if (task->isLocal()) {
                task->onEvent(*this, nullptr, m_controller->m_handle);
                consumeFrontIfSame(task);
                if (m_result_set) {
                    return SequenceProgress::kCompleted;
                }
                continue;
            }
            task->beforeSubmit();
            entry->context = task->contextBase();
            return SequenceProgress::kNeedWait;
        }
        return SequenceProgress::kCompleted;
    }

    SequenceProgress onActiveEvent(struct io_uring_cqe* cqe, GHandle handle) override {
        auto* entry = front();
        if (!entry) {
            return SequenceProgress::kCompleted;
        }
        auto* task = static_cast<TaskBase*>(entry->task);
        if (!task) {
            popFront();
            return prepareForSubmit();
        }
        if (task->onEvent(*this, cqe, handle)) {
            consumeFrontIfSame(task);
        }
        if (m_result_set) {
            return SequenceProgress::kCompleted;
        }
        return prepareForSubmit();
    }
#else
    SequenceProgress prepareForSubmit(GHandle handle) override {
        while (auto* entry = front()) {
            auto* task = static_cast<TaskBase*>(entry->task);
            if (!task) {
                popFront();
                continue;
            }
            task->beforeSubmit();
            entry->context = task->contextBase();
            if (task->onReady(*this, handle)) {
                consumeFrontIfSame(task);
                if (m_result_set) {
                    return SequenceProgress::kCompleted;
                }
                continue;
            }
            return SequenceProgress::kNeedWait;
        }
        return SequenceProgress::kCompleted;
    }

    SequenceProgress onActiveEvent(GHandle handle) override {
        auto* entry = front();
        if (!entry) {
            return SequenceProgress::kCompleted;
        }
        auto* task = static_cast<TaskBase*>(entry->task);
        if (!task) {
            popFront();
            return prepareForSubmit(handle);
        }
        if (task->onEvent(*this, handle)) {
            consumeFrontIfSame(task);
        }
        if (m_result_set) {
            return SequenceProgress::kCompleted;
        }
        return prepareForSubmit(handle);
    }
#endif

private:
    void emplaceTask(IOEventType type, IOContextBase* context, TaskBase* task) {
        if (m_size >= InlineN) {
            std::abort();
        }
        const size_t index = (m_head + m_size) % InlineN;
        m_tasks[index] = IOTask{type, task, context};
        ++m_size;
    }

    void consumeFrontIfSame(TaskBase* task) {
        auto* entry = front();
        if (entry && entry->task == task) {
            popFront();
        }
    }

    std::array<IOTask, InlineN> m_tasks{};  ///< 环形任务缓冲区
    size_t m_head = 0;  ///< 队首索引
    size_t m_size = 0;  ///< 当前排队任务数
    std::optional<ResultT> m_result;  ///< sequence 成功结果
    bool m_result_set = false;  ///< sequence 是否已经产出成功结果
};

/**
 * @brief 立即就绪的 awaitable
 * @tparam ResultT 返回值类型
 */
template <typename ResultT>
class ReadyAwaitable : public TimeoutSupport<ReadyAwaitable<ResultT>> {
public:
    using result_type = ResultT;  ///< await_resume() 返回值类型

    explicit ReadyAwaitable(ResultT ready_result)
        : m_ready_result(std::move(ready_result)) {}

    bool await_ready() const noexcept {
        return true;
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise>) const noexcept {
        return false;
    }

    auto await_resume() -> ResultT {
        return std::move(m_ready_result);
    }

private:
    ResultT m_ready_result;  ///< 预先准备好的结果值
};

/**
 * @brief 基于用户状态机的 sequence awaitable
 * @tparam MachineT 满足 AwaitableStateMachine 概念的状态机类型
 */
template <AwaitableStateMachine MachineT>
class StateMachineAwaitable
    : public SequenceAwaitableBase
    , public TimeoutSupport<StateMachineAwaitable<MachineT>> {
public:
    using result_type = typename MachineT::result_type;

    StateMachineAwaitable(IOController* controller, MachineT machine)
        : SequenceAwaitableBase(controller, detail::resolveStateMachineOwnerDomain(machine))
        , m_machine(std::move(machine))
        , m_recv_context(nullptr, 0)
        , m_readv_context(std::span<const struct iovec>{})
        , m_send_context(nullptr, 0)
        , m_writev_context(std::span<const struct iovec>{})
        , m_connect_context(Host{}) {}

private:
    template <typename ResultT, size_t InlineN, typename FlowT>
    friend class AwaitableBuilder;

    StateMachineAwaitable(IOController* controller,
                          MachineT machine,
                          SequenceOwnerDomain requested_domain)
        : SequenceAwaitableBase(controller, requested_domain)
        , m_machine(std::move(machine))
        , m_recv_context(nullptr, 0)
        , m_readv_context(std::span<const struct iovec>{})
        , m_send_context(nullptr, 0)
        , m_writev_context(std::span<const struct iovec>{})
        , m_connect_context(Host{}) {}

public:
    bool await_ready() {
        return m_result_set || m_error.has_value();
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        if (!m_context_bound) {
            detail::bindAwaitContextIfSupported(m_machine, detail::makeAwaitContext(handle));
            m_context_bound = true;
        }
        return SequenceAwaitableBase::await_suspend(handle);
    }

    auto await_resume() -> result_type {
        onCompleted();
        if (m_result_set) {
            return std::move(*m_result);
        }
        if (m_error.has_value()) {
            if constexpr (detail::is_expected_v<result_type>) {
                using ErrorT = typename detail::expected_traits<result_type>::error_type;
                if constexpr (std::is_constructible_v<ErrorT, IOError>) {
                    return std::unexpected(ErrorT(*m_error));
                }
            }
        }
        if constexpr (detail::is_expected_v<result_type>) {
            using ErrorT = typename detail::expected_traits<result_type>::error_type;
            if constexpr (std::is_constructible_v<ErrorT, IOError>) {
                return std::unexpected(ErrorT(IOError(kNotReady, errno)));
            }
        }
        std::abort();
    }

    IOTask* front() override {
        return m_has_active_task ? &m_active_task : nullptr;
    }

    const IOTask* front() const override {
        return m_has_active_task ? &m_active_task : nullptr;
    }

    void popFront() override {
        clearActiveTask();
    }

    bool empty() const override {
        return !m_has_active_task;
    }

    void markTimeout() {
        const IOError timeout_error(kTimeout, 0);
        const ActiveKind active_kind = m_active_kind;
        clearActiveTask();

        switch (active_kind) {
        case ActiveKind::kRead:
        case ActiveKind::kReadv:
            m_machine.onRead(std::unexpected(timeout_error));
            break;
        case ActiveKind::kWrite:
        case ActiveKind::kWritev:
            m_machine.onWrite(std::unexpected(timeout_error));
            break;
        case ActiveKind::kConnect:
            deliverConnect(std::unexpected(timeout_error));
            break;
        case ActiveKind::kNone:
            break;
        }

        (void)pump();
        if (!m_result_set && !m_error.has_value()) {
            m_error = timeout_error;
        }
    }

#ifdef USE_IOURING
    SequenceProgress prepareForSubmit() override {
        return pump();
    }

    SequenceProgress onActiveEvent(struct io_uring_cqe* cqe, GHandle handle) override {
        if (!m_has_active_task) {
            return pump();
        }
        if (m_active_kind == ActiveKind::kRead) {
            if (!m_recv_context.handleComplete(cqe, handle)) {
                return SequenceProgress::kNeedWait;
            }
            auto io_result = std::move(m_recv_context.m_result);
            clearActiveTask();
            m_machine.onRead(std::move(io_result));
            return pump();
        }
        if (m_active_kind == ActiveKind::kReadv) {
            if (!m_readv_context.handleComplete(cqe, handle)) {
                return SequenceProgress::kNeedWait;
            }
            auto io_result = std::move(m_readv_context.m_result);
            clearActiveTask();
            m_machine.onRead(std::move(io_result));
            return pump();
        }
        if (m_active_kind == ActiveKind::kWrite) {
            if (!m_send_context.handleComplete(cqe, handle)) {
                return SequenceProgress::kNeedWait;
            }
            auto io_result = std::move(m_send_context.m_result);
            clearActiveTask();
            m_machine.onWrite(std::move(io_result));
            return pump();
        }
        if (m_active_kind == ActiveKind::kWritev) {
            if (!m_writev_context.handleComplete(cqe, handle)) {
                return SequenceProgress::kNeedWait;
            }
            auto io_result = std::move(m_writev_context.m_result);
            clearActiveTask();
            m_machine.onWrite(std::move(io_result));
            return pump();
        }
        if (m_active_kind == ActiveKind::kConnect) {
            if (!m_connect_context.handleComplete(cqe, handle)) {
                return SequenceProgress::kNeedWait;
            }
            auto io_result = std::move(m_connect_context.m_result);
            clearActiveTask();
            deliverConnect(std::move(io_result));
            return pump();
        }
        m_error = IOError(kParamInvalid, 0);
        return SequenceProgress::kCompleted;
    }
#else
    SequenceProgress prepareForSubmit(GHandle handle) override {
        for (size_t i = 0; i < kInlineTransitionCap; ++i) {
            const SequenceProgress progress = pump();
            if (progress == SequenceProgress::kCompleted) {
                return progress;
            }
            if (!m_has_active_task) {
                return SequenceProgress::kCompleted;
            }
            if (m_active_kind == ActiveKind::kRead) {
                if (!m_recv_context.handleComplete(handle)) {
                    return SequenceProgress::kNeedWait;
                }
                auto io_result = std::move(m_recv_context.m_result);
                clearActiveTask();
                m_machine.onRead(std::move(io_result));
                continue;
            }
            if (m_active_kind == ActiveKind::kReadv) {
                if (!m_readv_context.handleComplete(handle)) {
                    return SequenceProgress::kNeedWait;
                }
                auto io_result = std::move(m_readv_context.m_result);
                clearActiveTask();
                m_machine.onRead(std::move(io_result));
                continue;
            }
            if (m_active_kind == ActiveKind::kWrite) {
                if (!m_send_context.handleComplete(handle)) {
                    return SequenceProgress::kNeedWait;
                }
                auto io_result = std::move(m_send_context.m_result);
                clearActiveTask();
                m_machine.onWrite(std::move(io_result));
                continue;
            }
            if (m_active_kind == ActiveKind::kWritev) {
                if (!m_writev_context.handleComplete(handle)) {
                    return SequenceProgress::kNeedWait;
                }
                auto io_result = std::move(m_writev_context.m_result);
                clearActiveTask();
                m_machine.onWrite(std::move(io_result));
                continue;
            }
            if (m_active_kind == ActiveKind::kConnect) {
                if (!m_connect_context.handleComplete(handle)) {
                    return SequenceProgress::kNeedWait;
                }
                auto io_result = std::move(m_connect_context.m_result);
                clearActiveTask();
                deliverConnect(std::move(io_result));
                continue;
            }
            m_error = IOError(kParamInvalid, 0);
            return SequenceProgress::kCompleted;
        }
        m_error = IOError(kParamInvalid, 0);
        clearActiveTask();
        return SequenceProgress::kCompleted;
    }

    SequenceProgress onActiveEvent(GHandle handle) override {
        if (!m_has_active_task) {
            return prepareForSubmit(handle);
        }
        if (m_active_kind == ActiveKind::kRead) {
            if (!m_recv_context.handleComplete(handle)) {
                return SequenceProgress::kNeedWait;
            }
            auto io_result = std::move(m_recv_context.m_result);
            clearActiveTask();
            m_machine.onRead(std::move(io_result));
            return prepareForSubmit(handle);
        }
        if (m_active_kind == ActiveKind::kReadv) {
            if (!m_readv_context.handleComplete(handle)) {
                return SequenceProgress::kNeedWait;
            }
            auto io_result = std::move(m_readv_context.m_result);
            clearActiveTask();
            m_machine.onRead(std::move(io_result));
            return prepareForSubmit(handle);
        }
        if (m_active_kind == ActiveKind::kWrite) {
            if (!m_send_context.handleComplete(handle)) {
                return SequenceProgress::kNeedWait;
            }
            auto io_result = std::move(m_send_context.m_result);
            clearActiveTask();
            m_machine.onWrite(std::move(io_result));
            return prepareForSubmit(handle);
        }
        if (m_active_kind == ActiveKind::kWritev) {
            if (!m_writev_context.handleComplete(handle)) {
                return SequenceProgress::kNeedWait;
            }
            auto io_result = std::move(m_writev_context.m_result);
            clearActiveTask();
            m_machine.onWrite(std::move(io_result));
            return prepareForSubmit(handle);
        }
        if (m_active_kind == ActiveKind::kConnect) {
            if (!m_connect_context.handleComplete(handle)) {
                return SequenceProgress::kNeedWait;
            }
            auto io_result = std::move(m_connect_context.m_result);
            clearActiveTask();
            deliverConnect(std::move(io_result));
            return prepareForSubmit(handle);
        }
        m_error = IOError(kParamInvalid, 0);
        return SequenceProgress::kCompleted;
    }
#endif

private:
    /**
     * @brief 当前活动 IO 步骤类型
     */
    enum class ActiveKind {
        kNone,      ///< 当前没有活动 IO
        kRead,      ///< 当前活动步骤为 recv/read
        kReadv,     ///< 当前活动步骤为 readv
        kWrite,     ///< 当前活动步骤为 send/write
        kWritev,    ///< 当前活动步骤为 writev
        kConnect,   ///< 当前活动步骤为 connect
    };

    static constexpr size_t kInlineTransitionCap = 64;

    SequenceProgress pump() {
        for (size_t i = 0; i < kInlineTransitionCap; ++i) {
            if (m_result_set || m_error.has_value()) {
                return SequenceProgress::kCompleted;
            }
            if (m_has_active_task) {
                return SequenceProgress::kNeedWait;
            }

            auto action = m_machine.advance();
            switch (action.signal) {
            case MachineSignal::kContinue:
                continue;
            case MachineSignal::kWaitRead:
                if (action.read_buffer == nullptr && action.read_length != 0) {
                    m_error = IOError(kParamInvalid, 0);
                    clearActiveTask();
                    return SequenceProgress::kCompleted;
                }
                if (action.read_length == 0) {
                    m_machine.onRead(std::expected<size_t, IOError>(size_t{0}));
                    continue;
                }
                m_recv_context.m_buffer = action.read_buffer;
                m_recv_context.m_length = action.read_length;
                m_active_task = IOTask{RECV, nullptr, &m_recv_context};
                m_has_active_task = true;
                m_active_kind = ActiveKind::kRead;
                return SequenceProgress::kNeedWait;
            case MachineSignal::kWaitReadv:
                if (action.iovecs == nullptr && action.iov_count != 0) {
                    m_error = IOError(kParamInvalid, 0);
                    clearActiveTask();
                    return SequenceProgress::kCompleted;
                }
                if (action.iov_count == 0) {
                    m_machine.onRead(std::expected<size_t, IOError>(size_t{0}));
                    continue;
                }
                m_readv_context.m_iovecs = std::span<const struct iovec>(action.iovecs, action.iov_count);
#ifdef USE_IOURING
                m_readv_context.initMsghdr();
#endif
                m_active_task = IOTask{READV, nullptr, &m_readv_context};
                m_has_active_task = true;
                m_active_kind = ActiveKind::kReadv;
                return SequenceProgress::kNeedWait;
            case MachineSignal::kWaitWrite:
                if (action.write_buffer == nullptr && action.write_length != 0) {
                    m_error = IOError(kParamInvalid, 0);
                    clearActiveTask();
                    return SequenceProgress::kCompleted;
                }
                if (action.write_length == 0) {
                    m_machine.onWrite(std::expected<size_t, IOError>(size_t{0}));
                    continue;
                }
                m_send_context.m_buffer = action.write_buffer;
                m_send_context.m_length = action.write_length;
                m_active_task = IOTask{SEND, nullptr, &m_send_context};
                m_has_active_task = true;
                m_active_kind = ActiveKind::kWrite;
                return SequenceProgress::kNeedWait;
            case MachineSignal::kWaitWritev:
                if (action.iovecs == nullptr && action.iov_count != 0) {
                    m_error = IOError(kParamInvalid, 0);
                    clearActiveTask();
                    return SequenceProgress::kCompleted;
                }
                if (action.iov_count == 0) {
                    m_machine.onWrite(std::expected<size_t, IOError>(size_t{0}));
                    continue;
                }
                m_writev_context.m_iovecs = std::span<const struct iovec>(action.iovecs, action.iov_count);
#ifdef USE_IOURING
                m_writev_context.initMsghdr();
#endif
                m_active_task = IOTask{WRITEV, nullptr, &m_writev_context};
                m_has_active_task = true;
                m_active_kind = ActiveKind::kWritev;
                return SequenceProgress::kNeedWait;
            case MachineSignal::kWaitConnect:
                m_connect_context.m_host = action.connect_host;
                m_active_task = IOTask{CONNECT, nullptr, &m_connect_context};
                m_has_active_task = true;
                m_active_kind = ActiveKind::kConnect;
                return SequenceProgress::kNeedWait;
            case MachineSignal::kComplete:
                if (!action.result.has_value()) {
                    m_error = IOError(kParamInvalid, 0);
                    clearActiveTask();
                    return SequenceProgress::kCompleted;
                }
                m_result = std::move(*action.result);
                m_result_set = true;
                clearActiveTask();
                return SequenceProgress::kCompleted;
            case MachineSignal::kFail:
                m_error = action.error.value_or(IOError(kParamInvalid, 0));
                clearActiveTask();
                return SequenceProgress::kCompleted;
            }
        }
        m_error = IOError(kParamInvalid, 0);
        clearActiveTask();
        return SequenceProgress::kCompleted;
    }

    void deliverConnect(std::expected<void, IOError> result) {
        if constexpr (requires(MachineT& machine, std::expected<void, IOError> connect_result) {
            { machine.onConnect(std::move(connect_result)) } -> std::same_as<void>;
        }) {
            m_machine.onConnect(std::move(result));
        } else {
            if (!result) {
                m_error = result.error();
            } else {
                m_error = IOError(kParamInvalid, 0);
            }
        }
    }

    void clearActiveTask() {
        m_active_task = IOTask{};
        m_has_active_task = false;
        m_active_kind = ActiveKind::kNone;
    }

    MachineT m_machine;  ///< 用户提供的状态机对象
    RecvIOContext m_recv_context;  ///< 复用的 recv 上下文
    ReadvIOContext m_readv_context;  ///< 复用的 readv 上下文
    SendIOContext m_send_context;  ///< 复用的 send 上下文
    WritevIOContext m_writev_context;  ///< 复用的 writev 上下文
    ConnectIOContext m_connect_context;  ///< 复用的 connect 上下文
    IOTask m_active_task{};  ///< 当前已激活的 sequence 任务
    bool m_has_active_task = false;  ///< 当前是否已有活动任务
    ActiveKind m_active_kind = ActiveKind::kNone;  ///< 当前活动任务类型
    bool m_context_bound = false;  ///< 是否已把 AwaitContext 绑定给状态机
    std::optional<result_type> m_result;  ///< 状态机成功结果
    bool m_result_set = false;  ///< 状态机是否已产出成功结果
};

/**
 * @brief 状态机构造器
 * @tparam MachineT 状态机类型
 */
template <AwaitableStateMachine MachineT>
class StateMachineBuilder {
public:
    StateMachineBuilder(IOController* controller, MachineT machine)
        : m_controller(controller)
        , m_machine(std::move(machine)) {}

    auto build() & -> StateMachineAwaitable<MachineT> {
        return StateMachineAwaitable<MachineT>(m_controller, std::move(m_machine));
    }

    auto build() && -> StateMachineAwaitable<MachineT> {
        return StateMachineAwaitable<MachineT>(m_controller, std::move(m_machine));
    }

private:
    IOController* m_controller;  ///< 关联的 IO 控制器
    MachineT m_machine;  ///< 用户提供的状态机实例
};

/**
 * @brief 绑定 IOContext 的 sequence 步骤
 * @tparam ResultT sequence 结果类型
 * @tparam InlineN 内联任务容量
 * @tparam FlowT 宿主 flow 类型
 * @tparam BaseContextT 具体 IOContext 类型
 * @tparam Handler 宿主 flow 上的回调成员函数
 */
template <typename ResultT, size_t InlineN, typename FlowT, typename BaseContextT, auto Handler>
struct SequenceStep : public SequenceAwaitable<ResultT, InlineN>::TaskBase, public BaseContextT {  ///< 绑定具体 IOContext 的 sequence 步骤实现
    static_assert(std::is_base_of_v<IOContextBase, BaseContextT>,
                  "SequenceStep requires an IOContextBase-derived base context");

    template <typename... Args>
    explicit SequenceStep(FlowT* owner, Args&&... args)
        : BaseContextT(std::forward<Args>(args)...)
        , m_owner(owner) {}

    IOContextBase* contextBase() override {
        return this;
    }

    IOEventType defaultEventType() const override {
        return detail::customAwaitableDefaultEvent<BaseContextT>();
    }

    bool isLocal() const override {
        return false;
    }

#ifdef USE_IOURING
    bool onEvent(SequenceAwaitable<ResultT, InlineN>& owner, struct io_uring_cqe* cqe, GHandle handle) override {
        if (!BaseContextT::handleComplete(cqe, handle)) {
            return false;
        }
        auto ops = owner.ops();
        (m_owner->*Handler)(ops, static_cast<BaseContextT&>(*this));
        return true;
    }
#else
    bool onReady(SequenceAwaitable<ResultT, InlineN>& owner, GHandle handle) override {
        if (!BaseContextT::handleComplete(handle)) {
            return false;
        }
        auto ops = owner.ops();
        (m_owner->*Handler)(ops, static_cast<BaseContextT&>(*this));
        return true;
    }

    bool onEvent(SequenceAwaitable<ResultT, InlineN>& owner, GHandle handle) override {
        if (!BaseContextT::handleComplete(handle)) {
            return false;
        }
        auto ops = owner.ops();
        (m_owner->*Handler)(ops, static_cast<BaseContextT&>(*this));
        return true;
    }
#endif

private:
    FlowT* m_owner;  ///< 宿主 flow 对象
};

/**
 * @brief 纯本地 sequence 步骤
 */
template <typename ResultT, size_t InlineN, typename FlowT, auto Handler>
struct LocalSequenceStep : public SequenceAwaitable<ResultT, InlineN>::TaskBase {
    explicit LocalSequenceStep(FlowT* owner)
        : m_owner(owner) {}

    IOContextBase* contextBase() override {
        return nullptr;
    }

    IOEventType defaultEventType() const override {
        return IOEventType::INVALID;
    }

    bool isLocal() const override {
        return true;
    }

#ifdef USE_IOURING
    bool onEvent(SequenceAwaitable<ResultT, InlineN>& owner, struct io_uring_cqe*, GHandle) override {
        auto ops = owner.ops();
        (m_owner->*Handler)(ops);
        return true;
    }
#else
    bool onReady(SequenceAwaitable<ResultT, InlineN>& owner, GHandle) override {
        auto ops = owner.ops();
        (m_owner->*Handler)(ops);
        return true;
    }

    bool onEvent(SequenceAwaitable<ResultT, InlineN>& owner, GHandle) override {
        auto ops = owner.ops();
        (m_owner->*Handler)(ops);
        return true;
    }
#endif

private:
    FlowT* m_owner;  ///< 宿主 flow 对象
};

/**
 * @brief 带可重挂起接收逻辑的 parser 步骤
 */
template <typename ResultT, size_t InlineN, typename FlowT, auto Handler>
struct ParserSequenceStep : public SequenceAwaitable<ResultT, InlineN>::TaskBase {
    explicit ParserSequenceStep(FlowT* owner,
                                typename SequenceAwaitable<ResultT, InlineN>::TaskBase* rearm_step)
        : m_owner(owner)
        , m_rearm_step(rearm_step) {}

    IOContextBase* contextBase() override {
        return nullptr;
    }

    IOEventType defaultEventType() const override {
        return IOEventType::INVALID;
    }

    bool isLocal() const override {
        return true;
    }

#ifdef USE_IOURING
    bool onEvent(SequenceAwaitable<ResultT, InlineN>& owner, struct io_uring_cqe*, GHandle) override {
        return run(owner);
    }
#else
    bool onReady(SequenceAwaitable<ResultT, InlineN>& owner, GHandle) override {
        return run(owner);
    }

    bool onEvent(SequenceAwaitable<ResultT, InlineN>& owner, GHandle) override {
        return run(owner);
    }
#endif

private:
    bool run(SequenceAwaitable<ResultT, InlineN>& owner) {
        auto ops = owner.ops();
        const ParseStatus status = (m_owner->*Handler)(ops);
        switch (status) {
            case ParseStatus::kNeedMore:
                if (m_rearm_step == nullptr || m_rearm_step->isLocal()) {
                    owner.fail(IOError(kParamInvalid, 0));
                    return true;
                }
                owner.queue(*m_rearm_step);
                owner.queue(*this);
                return true;
            case ParseStatus::kContinue:
                owner.queue(*this);
                return true;
            case ParseStatus::kCompleted:
                return true;
        }
        owner.fail(IOError(kParamInvalid, 0));
        return true;
    }

    FlowT* m_owner;  ///< 宿主 flow 对象
    typename SequenceAwaitable<ResultT, InlineN>::TaskBase* m_rearm_step;  ///< NeedMore 时重新排队的接收步骤
};

namespace detail {

/**
 * @brief 线性状态机实现
 * @tparam ResultT 线性状态机结果类型
 * @tparam InlineN 内联 sequence 容量
 * @tparam FlowT 宿主 flow 类型
 */
template <typename ResultT, size_t InlineN, typename FlowT>
class LinearMachine {
public:
    using result_type = ResultT;  ///< 最终结果类型
    using OpsT = SequenceOps<ResultT, InlineN>;  ///< 运行时可用的操作视图

    static constexpr size_t kInvalidIndex = static_cast<size_t>(-1);  ///< 无效节点索引哨兵值

    /**
     * @brief 线性状态机节点类型
     */
    enum class NodeKind : uint8_t {
        kRecv,     ///< recv 节点
        kReadv,    ///< readv 节点
        kSend,     ///< send 节点
        kWritev,   ///< writev 节点
        kConnect,  ///< connect 节点
        kParse,    ///< parser 节点
        kLocal,    ///< 本地同步节点
        kFinish,   ///< 结束节点
    };

    using IOHandlerFn = void(*)(FlowT*, OpsT&, IOContextBase&);
    using LocalHandlerFn = void(*)(FlowT*, OpsT&);
    using ParseHandlerFn = ParseStatus(*)(FlowT*, OpsT&);

    /**
     * @brief 单个线性状态机节点描述
     */
    struct Node {
        NodeKind kind = NodeKind::kLocal;  ///< 节点类型
        IOHandlerFn io_handler = nullptr;  ///< IO 节点回调
        LocalHandlerFn local_handler = nullptr;  ///< 本地节点回调
        ParseHandlerFn parse_handler = nullptr;  ///< parser 节点回调
        char* read_buffer = nullptr;  ///< recv 缓冲区
        const char* write_buffer = nullptr;  ///< send 缓冲区
        const struct iovec* iovecs = nullptr;  ///< readv/writev iovec 指针
        size_t iov_count = 0;  ///< iovec 数量
        size_t io_length = 0;  ///< 单缓冲 IO 请求长度
        Host connect_host{};  ///< connect 目标地址
        size_t parse_rearm_recv_index = kInvalidIndex;  ///< parser NeedMore 时重挂起的接收节点索引
    };

    using NodeList = std::vector<Node>;

    LinearMachine(IOController* controller, FlowT* flow, NodeList nodes)
        : m_controller(controller)
        , m_flow(flow)
        , m_nodes(std::move(nodes))
        , m_ops_owner(nullptr)
        , m_recv_context(nullptr, 0)
        , m_readv_context(std::span<const struct iovec>{})
        , m_send_context(nullptr, 0)
        , m_writev_context(std::span<const struct iovec>{})
        , m_connect_context(Host{}) {}

    template <auto Handler>
    static Node makeRecvNode(char* buffer, size_t length) {
        Node node;
        node.kind = NodeKind::kRecv;
        node.io_handler = &invokeIO<RecvIOContext, Handler>;
        node.read_buffer = buffer;
        node.io_length = length;
        return node;
    }

    template <auto Handler>
    static Node makeSendNode(const char* buffer, size_t length) {
        Node node;
        node.kind = NodeKind::kSend;
        node.io_handler = &invokeIO<SendIOContext, Handler>;
        node.write_buffer = buffer;
        node.io_length = length;
        return node;
    }

    template <auto Handler>
    static Node makeReadvNode(const struct iovec* iovecs, size_t count) {
        Node node;
        node.kind = NodeKind::kReadv;
        node.io_handler = &invokeIO<ReadvIOContext, Handler>;
        node.iovecs = iovecs;
        node.iov_count = count;
        return node;
    }

    template <auto Handler>
    static Node makeWritevNode(const struct iovec* iovecs, size_t count) {
        Node node;
        node.kind = NodeKind::kWritev;
        node.io_handler = &invokeIO<WritevIOContext, Handler>;
        node.iovecs = iovecs;
        node.iov_count = count;
        return node;
    }

    template <auto Handler>
    static Node makeConnectNode(const Host& host) {
        Node node;
        node.kind = NodeKind::kConnect;
        node.io_handler = &invokeIO<ConnectIOContext, Handler>;
        node.connect_host = host;
        return node;
    }

    template <auto Handler>
    static Node makeLocalNode() {
        Node node;
        node.kind = NodeKind::kLocal;
        node.local_handler = &invokeLocal<Handler>;
        return node;
    }

    template <auto Handler>
    static Node makeFinishNode() {
        Node node;
        node.kind = NodeKind::kFinish;
        node.local_handler = &invokeLocal<Handler>;
        return node;
    }

    template <auto Handler>
    static Node makeParseNode(size_t rearm_recv_index) {
        Node node;
        node.kind = NodeKind::kParse;
        node.parse_handler = &invokeParse<Handler>;
        node.parse_rearm_recv_index = rearm_recv_index;
        return node;
    }

    void onAwaitContext(const AwaitContext& ctx) {
        if constexpr (requires(FlowT& flow, const AwaitContext& context) {
            flow.onAwaitContext(context);
        }) {
            if (m_flow != nullptr) {
                m_flow->onAwaitContext(ctx);
            }
        }
    }

    MachineAction<result_type> advance() {
        if (m_result.has_value()) {
            return MachineAction<result_type>::complete(std::move(*m_result));
        }
        if (m_error.has_value()) {
            return MachineAction<result_type>::fail(*m_error);
        }
        if (m_cursor >= m_nodes.size()) {
            setIOError(IOError(kNotReady, 0));
            return emitActionFromOutcome();
        }

        const Node& node = m_nodes[m_cursor];
        switch (node.kind) {
        case NodeKind::kRecv:
            m_recv_context.m_buffer = node.read_buffer;
            m_recv_context.m_length = node.io_length;
            m_pending_io = PendingIO::kRead;
            m_pending_index = m_cursor;
            return MachineAction<result_type>::waitRead(node.read_buffer, node.io_length);
        case NodeKind::kReadv:
            m_readv_context.m_iovecs = std::span<const struct iovec>(node.iovecs, node.iov_count);
#ifdef USE_IOURING
            m_readv_context.initMsghdr();
#endif
            m_pending_io = PendingIO::kReadv;
            m_pending_index = m_cursor;
            return MachineAction<result_type>::waitReadv(node.iovecs, node.iov_count);
        case NodeKind::kSend:
            m_send_context.m_buffer = node.write_buffer;
            m_send_context.m_length = node.io_length;
            m_pending_io = PendingIO::kWrite;
            m_pending_index = m_cursor;
            return MachineAction<result_type>::waitWrite(node.write_buffer, node.io_length);
        case NodeKind::kWritev:
            m_writev_context.m_iovecs = std::span<const struct iovec>(node.iovecs, node.iov_count);
#ifdef USE_IOURING
            m_writev_context.initMsghdr();
#endif
            m_pending_io = PendingIO::kWritev;
            m_pending_index = m_cursor;
            return MachineAction<result_type>::waitWritev(node.iovecs, node.iov_count);
        case NodeKind::kConnect:
            return runConnect(node);
        case NodeKind::kParse:
            return runParse(node);
        case NodeKind::kLocal:
        case NodeKind::kFinish:
            return runLocal(node);
        }
        setIOError(IOError(kParamInvalid, 0));
        return emitActionFromOutcome();
    }

    void onRead(std::expected<size_t, IOError> result) {
        if ((m_pending_io != PendingIO::kRead && m_pending_io != PendingIO::kReadv) ||
            m_pending_index >= m_nodes.size()) {
            setIOError(IOError(kParamInvalid, 0));
            return;
        }

        const bool has_value = result.has_value();
        std::optional<IOError> io_error;
        if (!has_value) {
            io_error = result.error();
        }
        const Node& node = m_nodes[m_pending_index];
        if (m_pending_io == PendingIO::kRead) {
            m_recv_context.m_result = std::move(result);
            invokeIONode(node, m_recv_context);
        } else {
            m_readv_context.m_result = std::move(result);
            invokeIONode(node, m_readv_context);
        }
        clearPendingIO();

        if (absorbOpsOutcome()) {
            return;
        }
        if (io_error.has_value()) {
            setIOError(std::move(*io_error));
            return;
        }
        ++m_cursor;
    }

    void onWrite(std::expected<size_t, IOError> result) {
        if ((m_pending_io != PendingIO::kWrite && m_pending_io != PendingIO::kWritev) ||
            m_pending_index >= m_nodes.size()) {
            setIOError(IOError(kParamInvalid, 0));
            return;
        }

        const bool has_value = result.has_value();
        std::optional<IOError> io_error;
        if (!has_value) {
            io_error = result.error();
        }
        const Node& node = m_nodes[m_pending_index];
        if (m_pending_io == PendingIO::kWrite) {
            m_send_context.m_result = std::move(result);
            invokeIONode(node, m_send_context);
        } else {
            m_writev_context.m_result = std::move(result);
            invokeIONode(node, m_writev_context);
        }
        clearPendingIO();

        if (absorbOpsOutcome()) {
            return;
        }
        if (io_error.has_value()) {
            setIOError(std::move(*io_error));
            return;
        }
        ++m_cursor;
    }

    void onConnect(std::expected<void, IOError> result) {
        if (m_pending_io != PendingIO::kConnect || m_pending_index >= m_nodes.size()) {
            setIOError(IOError(kParamInvalid, 0));
            return;
        }

        const bool has_value = result.has_value();
        std::optional<IOError> io_error;
        if (!has_value) {
            io_error = result.error();
        }
        m_connect_context.m_result = std::move(result);

        const Node& node = m_nodes[m_pending_index];
        invokeIONode(node, m_connect_context);
        clearPendingIO();

        if (absorbOpsOutcome()) {
            return;
        }
        if (io_error.has_value()) {
            setIOError(std::move(*io_error));
            return;
        }
        ++m_cursor;
    }

private:
    /**
     * @brief 当前挂起中的 IO 类型
     */
    enum class PendingIO : uint8_t {
        kNone,     ///< 当前没有挂起 IO
        kRead,     ///< 当前挂起 recv/read
        kReadv,    ///< 当前挂起 readv
        kWrite,    ///< 当前挂起 send/write
        kWritev,   ///< 当前挂起 writev
        kConnect,  ///< 当前挂起 connect
    };

    template <typename ContextT, auto Handler>
    static void invokeIO(FlowT* flow, OpsT& ops, IOContextBase& context) {
        (flow->*Handler)(ops, static_cast<ContextT&>(context));
    }

    template <auto Handler>
    static void invokeLocal(FlowT* flow, OpsT& ops) {
        (flow->*Handler)(ops);
    }

    template <auto Handler>
    static ParseStatus invokeParse(FlowT* flow, OpsT& ops) {
        return (flow->*Handler)(ops);
    }

    MachineAction<result_type> runConnect(const Node& node) {
        if (node.io_handler == nullptr) {
            setIOError(IOError(kParamInvalid, 0));
            return emitActionFromOutcome();
        }

        m_connect_context.m_host = node.connect_host;
        m_pending_io = PendingIO::kConnect;
        m_pending_index = m_cursor;
        return MachineAction<result_type>::waitConnect(node.connect_host);
    }

    MachineAction<result_type> runLocal(const Node& node) {
        if (node.local_handler == nullptr) {
            setIOError(IOError(kParamInvalid, 0));
            return emitActionFromOutcome();
        }

        m_ops_owner.resetOutcomeForReuse();
        auto ops = m_ops_owner.ops();
        node.local_handler(m_flow, ops);

        if (absorbOpsOutcome()) {
            return emitActionFromOutcome();
        }
        ++m_cursor;
        return MachineAction<result_type>::continue_();
    }

    MachineAction<result_type> runParse(const Node& node) {
        if (node.parse_handler == nullptr) {
            setIOError(IOError(kParamInvalid, 0));
            return emitActionFromOutcome();
        }

        m_ops_owner.resetOutcomeForReuse();
        auto ops = m_ops_owner.ops();
        const ParseStatus status = node.parse_handler(m_flow, ops);

        if (absorbOpsOutcome()) {
            return emitActionFromOutcome();
        }

        switch (status) {
        case ParseStatus::kNeedMore:
            if (node.parse_rearm_recv_index == kInvalidIndex ||
                node.parse_rearm_recv_index >= m_nodes.size() ||
                (m_nodes[node.parse_rearm_recv_index].kind != NodeKind::kRecv &&
                 m_nodes[node.parse_rearm_recv_index].kind != NodeKind::kReadv)) {
                setIOError(IOError(kParamInvalid, 0));
                return emitActionFromOutcome();
            }
            m_cursor = node.parse_rearm_recv_index;
            return MachineAction<result_type>::continue_();
        case ParseStatus::kContinue:
            return MachineAction<result_type>::continue_();
        case ParseStatus::kCompleted:
            ++m_cursor;
            return MachineAction<result_type>::continue_();
        }
        setIOError(IOError(kParamInvalid, 0));
        return emitActionFromOutcome();
    }

    void invokeIONode(const Node& node, IOContextBase& context) {
        if (node.io_handler == nullptr) {
            setIOError(IOError(kParamInvalid, 0));
            return;
        }
        m_ops_owner.resetOutcomeForReuse();
        auto ops = m_ops_owner.ops();
        node.io_handler(m_flow, ops, context);
    }

    bool absorbOpsOutcome() {
        if (m_ops_owner.hasResultValue()) {
            auto result = m_ops_owner.takeResultValue();
            if (result.has_value()) {
                m_result = std::move(*result);
            } else {
                setIOError(IOError(kParamInvalid, 0));
            }
            return true;
        }
        if (m_ops_owner.hasFailure()) {
            auto error = m_ops_owner.takeFailure();
            if (error.has_value()) {
                setIOError(std::move(*error));
            } else {
                setIOError(IOError(kParamInvalid, 0));
            }
            return true;
        }
        if (!m_ops_owner.empty()) {
            m_ops_owner.clear();
            setIOError(IOError(kParamInvalid, 0));
            return true;
        }
        return false;
    }

    MachineAction<result_type> emitActionFromOutcome() {
        if (m_result.has_value()) {
            return MachineAction<result_type>::complete(std::move(*m_result));
        }
        if (m_error.has_value()) {
            return MachineAction<result_type>::fail(*m_error);
        }
        return MachineAction<result_type>::continue_();
    }

    void clearPendingIO() {
        m_pending_io = PendingIO::kNone;
        m_pending_index = kInvalidIndex;
    }

    void setIOError(IOError error) {
        if constexpr (detail::is_expected_v<result_type>) {
            using ErrorT = typename detail::expected_traits<result_type>::error_type;
            if constexpr (std::is_constructible_v<ErrorT, IOError>) {
                m_result = std::unexpected(ErrorT(std::move(error)));
                return;
            }
        }
        m_error = std::move(error);
    }

    IOController* m_controller;  ///< 关联的 IO 控制器
    FlowT* m_flow;  ///< 宿主 flow 对象
    NodeList m_nodes;  ///< 线性节点列表
    size_t m_cursor = 0;  ///< 当前执行到的节点索引
    PendingIO m_pending_io = PendingIO::kNone;  ///< 当前挂起中的 IO 类型
    size_t m_pending_index = kInvalidIndex;  ///< 当前挂起节点索引

    SequenceAwaitable<ResultT, InlineN> m_ops_owner;  ///< 复用的 sequence 操作容器
    RecvIOContext m_recv_context;  ///< recv 上下文缓存
    ReadvIOContext m_readv_context;  ///< readv 上下文缓存
    SendIOContext m_send_context;  ///< send 上下文缓存
    WritevIOContext m_writev_context;  ///< writev 上下文缓存
    ConnectIOContext m_connect_context;  ///< connect 上下文缓存

    std::optional<result_type> m_result;  ///< 成功结果
    std::optional<IOError> m_error;  ///< 错误结果
};

} // namespace detail

/**
 * @brief awaitable 构造器
 * @tparam ResultT awaitable 结果类型
 * @tparam InlineN 线性状态机和 sequence 的内联容量
 * @tparam FlowT 宿主 flow 类型；默认为无宿主
 */
template <typename ResultT, size_t InlineN = 4, typename FlowT = void>
class AwaitableBuilder {
public:
    using MachineT = detail::LinearMachine<ResultT, InlineN, FlowT>;  ///< 内部线性状态机类型
    using MachineNode = typename MachineT::Node;  ///< 线性状态机节点类型

    AwaitableBuilder(IOController* controller, FlowT& flow)
        : m_controller(controller)
        , m_flow(&flow)
    {
        m_nodes.reserve(InlineN);
    }

    template <AwaitableStateMachine MachineTParam>
    static auto fromStateMachine(IOController* controller, MachineTParam machine) -> StateMachineBuilder<MachineTParam> {
        static_assert(std::same_as<typename MachineTParam::result_type, ResultT>,
                      "AwaitableBuilder::fromStateMachine requires matching result_type");
        return StateMachineBuilder<MachineTParam>(controller, std::move(machine));
    }

    template <typename ReadyT>
    requires std::constructible_from<ResultT, ReadyT&&>
    static auto ready(ReadyT&& result) -> ReadyAwaitable<ResultT> {
        return ReadyAwaitable<ResultT>(ResultT(std::forward<ReadyT>(result)));
    }

    template <auto Handler>
    AwaitableBuilder& local() {
        m_nodes.push_back(MachineT::template makeLocalNode<Handler>());
        return *this;
    }

    template <auto Handler>
    AwaitableBuilder& parse() {
        m_nodes.push_back(MachineT::template makeParseNode<Handler>(m_last_recv_index));
        return *this;
    }

    template <auto Handler>
    AwaitableBuilder& finish() {
        m_nodes.push_back(MachineT::template makeFinishNode<Handler>());
        return *this;
    }

    template <auto Handler>
    AwaitableBuilder& recv(char* buffer, size_t length) {
        m_nodes.push_back(MachineT::template makeRecvNode<Handler>(buffer, length));
        m_last_recv_index = m_nodes.size() - 1;
        return *this;
    }

    template <auto Handler, size_t N>
    AwaitableBuilder& readv(std::array<struct iovec, N>& iovecs, size_t count = N) {
        const size_t bounded = ReadvIOContext::validateBorrowedCountOrAbort(count, N, "readv");
        m_nodes.push_back(MachineT::template makeReadvNode<Handler>(iovecs.data(), bounded));
        m_last_recv_index = m_nodes.size() - 1;
        return *this;
    }

    template <auto Handler, size_t N>
    AwaitableBuilder& readv(struct iovec (&iovecs)[N], size_t count = N) {
        const size_t bounded = ReadvIOContext::validateBorrowedCountOrAbort(count, N, "readv");
        m_nodes.push_back(MachineT::template makeReadvNode<Handler>(iovecs, bounded));
        m_last_recv_index = m_nodes.size() - 1;
        return *this;
    }

    template <auto Handler>
    AwaitableBuilder& send(const char* buffer, size_t length) {
        m_nodes.push_back(MachineT::template makeSendNode<Handler>(buffer, length));
        return *this;
    }

    template <auto Handler, size_t N>
    AwaitableBuilder& writev(std::array<struct iovec, N>& iovecs, size_t count = N) {
        const size_t bounded = WritevIOContext::validateBorrowedCountOrAbort(count, N, "writev");
        m_nodes.push_back(MachineT::template makeWritevNode<Handler>(iovecs.data(), bounded));
        return *this;
    }

    template <auto Handler, size_t N>
    AwaitableBuilder& writev(struct iovec (&iovecs)[N], size_t count = N) {
        const size_t bounded = WritevIOContext::validateBorrowedCountOrAbort(count, N, "writev");
        m_nodes.push_back(MachineT::template makeWritevNode<Handler>(iovecs, bounded));
        return *this;
    }

    template <auto Handler>
    AwaitableBuilder& connect(const Host& host) {
        m_nodes.push_back(MachineT::template makeConnectNode<Handler>(host));
        return *this;
    }

    auto build() & -> StateMachineAwaitable<MachineT> {
        return buildImpl();
    }

    auto build() && -> StateMachineAwaitable<MachineT> {
        return buildImpl();
    }

private:
    auto buildImpl() -> StateMachineAwaitable<MachineT> {
        bool has_read = false;
        bool has_write = false;
        for (const auto& node : m_nodes) {
            if (node.kind == MachineT::NodeKind::kRecv ||
                node.kind == MachineT::NodeKind::kReadv) {
                has_read = true;
            } else if (node.kind == MachineT::NodeKind::kSend ||
                       node.kind == MachineT::NodeKind::kWritev ||
                       node.kind == MachineT::NodeKind::kConnect) {
                has_write = true;
            }
            if (has_read && has_write) {
                break;
            }
        }
        const SequenceOwnerDomain domain =
            has_read && has_write ? SequenceOwnerDomain::ReadWrite
            : has_read ? SequenceOwnerDomain::Read
            : has_write ? SequenceOwnerDomain::Write
                        : SequenceOwnerDomain::ReadWrite;
        return StateMachineAwaitable<MachineT>(
            m_controller,
            MachineT(m_controller, m_flow, std::move(m_nodes)),
            domain
        );
    }

    IOController* m_controller;  ///< 关联的 IO 控制器
    FlowT* m_flow;  ///< 宿主 flow 对象
    std::vector<MachineNode> m_nodes;  ///< 构造中的线性节点列表
    size_t m_last_recv_index = MachineT::kInvalidIndex;  ///< 最近一次接收节点索引，供 parser 重挂起使用
};

/**
 * @brief 无宿主 flow 的 awaitable builder 特化
 */
template <typename ResultT, size_t InlineN>
class AwaitableBuilder<ResultT, InlineN, void> {
public:
    template <AwaitableStateMachine MachineT>
    static auto fromStateMachine(IOController* controller, MachineT machine) -> StateMachineBuilder<MachineT> {
        static_assert(std::same_as<typename MachineT::result_type, ResultT>,
                      "AwaitableBuilder::fromStateMachine requires matching result_type");
        return StateMachineBuilder<MachineT>(controller, std::move(machine));
    }

    template <typename ReadyT>
    requires std::constructible_from<ResultT, ReadyT&&>
    static auto ready(ReadyT&& result) -> ReadyAwaitable<ResultT> {
        return ReadyAwaitable<ResultT>(ResultT(std::forward<ReadyT>(result)));
    }
};


} // namespace galay::kernel

#include "awaitable.inl"

#endif // GALAY_KERNEL_AWAITABLE_H
