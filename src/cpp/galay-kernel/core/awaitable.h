/**
 * @file awaitable.h
 * @brief 异步IO可等待对象
 * @author galay-kernel
 * @version 3.3.0
 *
 * @details 三层继承结构：
 * - AwaitableBase: 基类（m_sqe_type, virtual ~）
 * - IOContextBase: 中间层（virtual handle_complete 纯虚函数）
 *   - XxxIOContext: IO参数 + result + handle_complete 实现
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

#include "../common/defn.hpp"
#include "../common/error.h"
#include "../common/host.hpp"
#include "timeout.hpp"
#include "watch_defs.hpp"
#include "waker.h"
#if defined(USE_EPOLL) || defined(USE_IOURING)
#include "accept_operation.hpp"
#endif
#include <cerrno>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <span>
#include <array>
#include <deque>
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
 * @brief 转发 facade 的 CRTP 混入
 * @tparam Derived facade 类型
 * @tparam InnerT 传 `void` 时复用 Derived::m_inner；传具体类型时由 mixin
 *               内嵌拥有该 inner
 *
 * @details 统一实现 facade 的 await_ready / await_suspend / await_resume /
 *          mark_timeout 到 `m_inner` 的转发，消除各协议模块的复制粘贴样板。
 *          新的自定义 facade 推荐使用两参数形式，避免暴露内部成员；已有
 *          协议 facade 使用一参数形式收口重复转发。超时行为通过
 *          `TimeoutSupport<Derived, Policy>` 的模板策略显式选择。
 * @note 成员函数体在被调用时才实例化，Derived 届时已是完整类型。
 */
template <typename Derived, typename InnerT = void>
class ForwardingAwaitable;

/**
 * @brief CRTP forwarding facade for a derived type that owns `m_inner`.
 *
 * The one-argument form keeps the inner object in the facade.  `m_inner` must
 * be accessible to the mixin (public or explicitly befriended); private state
 * should use the owning two-argument form below instead.
 */
template <typename Derived>
class ForwardingAwaitable<Derived, void> {
protected:
    ForwardingAwaitable() noexcept = default;

    Derived& self() noexcept { return static_cast<Derived&>(*this); }
    const Derived& self() const noexcept { return static_cast<const Derived&>(*this); }

    decltype(auto) inner() noexcept { return (self().m_inner); }
    decltype(auto) inner() const noexcept { return (self().m_inner); }

public:
    decltype(auto) await_ready() noexcept(noexcept(inner().await_ready())) {
        return inner().await_ready();
    }

    template <typename Promise>
    decltype(auto) await_suspend(std::coroutine_handle<Promise> handle)
        noexcept(noexcept(inner().await_suspend(handle))) {
        return inner().await_suspend(handle);
    }

    decltype(auto) await_resume() noexcept(noexcept(inner().await_resume())) {
        return inner().await_resume();
    }

    template <typename D = Derived>
    requires requires(D& value) { value.m_inner.mark_timeout(); }
    void mark_timeout() noexcept(noexcept(inner().mark_timeout())) {
        inner().mark_timeout();
    }

    template <typename D = Derived>
    requires requires(D& value) {
        { value.m_inner.owns_io_registration() } -> std::convertible_to<bool>;
    }
    bool owns_io_registration() noexcept(noexcept(inner().owns_io_registration())) {
        return static_cast<bool>(inner().owns_io_registration());
    }

    template <typename TimerT, typename D = Derived>
    requires requires(D& value, TimerT&& timer) {
        value.m_inner.bind_timeout_timer(std::forward<TimerT>(timer));
    }
    void bind_timeout_timer(TimerT&& timer)
        noexcept(noexcept(inner().bind_timeout_timer(std::forward<TimerT>(timer)))) {
        inner().bind_timeout_timer(std::forward<TimerT>(timer));
    }
};

/**
 * @brief Owning forwarding facade for a standalone inner awaitable.
 *
 * This form is useful for new custom awaitables: it supplies the storage and
 * all forwarding operations, so the derived type only needs to expose its
 * own result-specific API.
 */
template <typename Derived, typename InnerT>
class ForwardingAwaitable {
protected:
    explicit ForwardingAwaitable(InnerT inner) noexcept(
        std::is_nothrow_move_constructible_v<InnerT>)
        : m_inner(std::move(inner)) {}

    InnerT& inner() noexcept { return m_inner; }
    const InnerT& inner() const noexcept { return m_inner; }

public:
    ForwardingAwaitable(const ForwardingAwaitable&) = default;
    ForwardingAwaitable& operator=(const ForwardingAwaitable&) = default;
    ForwardingAwaitable(ForwardingAwaitable&&) noexcept = default;
    ForwardingAwaitable& operator=(ForwardingAwaitable&&) noexcept = default;

    decltype(auto) await_ready() noexcept(noexcept(m_inner.await_ready())) {
        return m_inner.await_ready();
    }

    template <typename Promise>
    decltype(auto) await_suspend(std::coroutine_handle<Promise> handle)
        noexcept(noexcept(m_inner.await_suspend(handle))) {
        return m_inner.await_suspend(handle);
    }

    decltype(auto) await_resume() noexcept(noexcept(m_inner.await_resume())) {
        return m_inner.await_resume();
    }

    template <typename T = InnerT>
    requires requires(T& value) { value.mark_timeout(); }
    void mark_timeout() noexcept(noexcept(m_inner.mark_timeout())) {
        m_inner.mark_timeout();
    }

    template <typename T = InnerT>
    requires requires(T& value) {
        { value.owns_io_registration() } -> std::convertible_to<bool>;
    }
    bool owns_io_registration() noexcept(noexcept(m_inner.owns_io_registration())) {
        return static_cast<bool>(m_inner.owns_io_registration());
    }

    template <typename TimerT, typename T = InnerT>
    requires requires(T& value, TimerT&& timer) {
        value.bind_timeout_timer(std::forward<TimerT>(timer));
    }
    void bind_timeout_timer(TimerT&& timer)
        noexcept(noexcept(m_inner.bind_timeout_timer(std::forward<TimerT>(timer)))) {
        m_inner.bind_timeout_timer(std::forward<TimerT>(timer));
    }

protected:
    InnerT m_inner;
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
    /**
     * @brief 消费 CQE 并返回该操作是否已完成
     * @param cqe io_uring 完成队列条目
     * @param handle 句柄
     * @return 该 IO 操作已完成时返回 true，仍需继续处理时返回 false
     */
    virtual bool handle_complete(struct io_uring_cqe* cqe, GHandle handle) = 0;
#else
    virtual bool handle_complete(GHandle handle) = 0;  ///< 在传统后端上消费一次就绪事件并返回该操作是否已完成
#endif

    // SequenceAwaitable 调度时可由上下文动态指定下一次等待方向；
    // 返回 INVALID 表示沿用静态 task.type。
    /**
     * @brief 返回动态事件方向；INVALID 表示沿用静态事件类型
     * @return 当前对象的类型、状态或错误码
     */
    virtual IOEventType type() const { return IOEventType::INVALID; }
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

int register_io_scheduler_event(Scheduler* scheduler,
                             IOEventType event,
                             IOController* controller) noexcept;
int register_io_scheduler_close(Scheduler* scheduler,
                             IOController* controller) noexcept;

using SequenceInterestMask = uint8_t;

constexpr bool sequence_event_uses_slot(IOEventType type,
                                     IOController::Index slot) noexcept;

constexpr SequenceInterestMask sequence_slot_mask(IOController::Index slot) noexcept {
    return static_cast<SequenceInterestMask>(1u << static_cast<uint8_t>(slot));
}

constexpr SequenceInterestMask sequence_interest_mask(IOEventType type) noexcept {
    SequenceInterestMask mask = 0;
    if (sequence_event_uses_slot(type, IOController::READ)) {
        mask = static_cast<SequenceInterestMask>(mask | sequence_slot_mask(IOController::READ));
    }
    if (sequence_event_uses_slot(type, IOController::WRITE)) {
        mask = static_cast<SequenceInterestMask>(mask | sequence_slot_mask(IOController::WRITE));
    }
    return mask;
}

/**
 * @brief 汇总 controller 上所有 sequence awaitable 的关注位
 * @param controller IO 控制器
 * @return SequenceInterestMask 操作结果
 */
SequenceInterestMask collect_sequence_interest_mask(const IOController* controller) noexcept;
/**
 * @brief 重新计算并写回 controller 的 sequence 关注位
 * @param controller IO 控制器
 * @return SequenceInterestMask 操作结果
 */
SequenceInterestMask sync_sequence_interest_mask(IOController* controller) noexcept;
/**
 * @brief 清空 controller 的 sequence 关注位与 armed 位
 * @param controller IO 控制器
 * @return 无返回值
 */
void clear_sequence_interest_mask(IOController* controller) noexcept;

inline uint32_t normalize_awaitable_errno(int ret) noexcept {
    return (ret < 0 && ret != -1)
        ? static_cast<uint32_t>(-ret)
        : static_cast<uint32_t>(errno);
}

template <typename ResultT>
inline bool finalize_awaitable_add_result(int ret,
                                       IOErrorCode io_error,
                                       std::expected<ResultT, IOError>& result) {
    if (ret == 1) {
        return false;
    }
    if (ret < 0) {
        result = std::unexpected(IOError(io_error, normalize_awaitable_errno(ret)));
        return false;
    }
    return true;
}

template <IOEventType Event, typename AwaitableT>
inline auto resume_io_awaitable(AwaitableT& awaitable) -> decltype(std::move(awaitable.m_result)) {
    // 控制器为空说明操作在挂起前已短路返回（如 kClosed），无槽位需要清理
    if (awaitable.m_controller != nullptr) {
        const bool owns_read =
            sequence_event_uses_slot(Event, IOController::READ) &&
            awaitable.m_controller->m_awaitable[IOController::READ] == &awaitable;
        const bool owns_write =
            sequence_event_uses_slot(Event, IOController::WRITE) &&
            awaitable.m_controller->m_awaitable[IOController::WRITE] == &awaitable;
        if (owns_read || owns_write) {
            awaitable.m_controller->remove_awaitable(Event);
        }
    }
    return std::move(awaitable.m_result);
}

template <typename AwaitableT, IOEventType Event, IOErrorCode ErrorCode, typename Promise>
inline bool suspend_registered_awaitable(AwaitableT& awaitable, std::coroutine_handle<Promise> handle) {
    awaitable.m_waker = Waker(handle);
#ifdef USE_IOURING
    awaitable.m_sqe_type = Event;
#endif
    if (awaitable.m_controller == nullptr) {
        awaitable.m_result = std::unexpected(IOError(kClosed, 0));
        return false;
    }
    if ((sequence_event_uses_slot(Event, IOController::READ) &&
         awaitable.m_controller->m_sequence_owner[IOController::READ] != nullptr) ||
        (sequence_event_uses_slot(Event, IOController::WRITE) &&
         awaitable.m_controller->m_sequence_owner[IOController::WRITE] != nullptr)) {
        awaitable.m_result = std::unexpected(IOError(kNotReady, 0));
        return false;
    }
    awaitable.m_controller->fill_awaitable(Event, &awaitable);
    auto* scheduler = awaitable.m_waker.get_scheduler();
    if (scheduler == nullptr || scheduler->type() != kIOScheduler) {
        awaitable.m_result = std::unexpected(IOError(kNotRunningOnIOScheduler, errno));
        awaitable.m_controller->remove_awaitable(Event);
        return false;
    }
    const int ret = register_io_scheduler_event(scheduler, Event, awaitable.m_controller);
    if (ret == 1) {
        awaitable.m_controller->remove_awaitable(Event);
        return false;
    }
    if (ret < 0) {
        awaitable.m_result = std::unexpected(IOError(ErrorCode, normalize_awaitable_errno(ret)));
        awaitable.m_controller->remove_awaitable(Event);
        return false;
    }
    return true;
}

template <typename Promise>
inline bool suspend_sequence_awaitable(SequenceAwaitableBase& awaitable,
                                     std::coroutine_handle<Promise> handle);

template <typename Promise>
inline AwaitContext make_await_context(std::coroutine_handle<Promise> handle) {
    TaskRef task = handle.promise().task_ref_view();
    return AwaitContext{
        .task = task,
        .scheduler = task.belong_scheduler(),
    };
}

template <typename TargetT>
inline void bind_await_context_if_supported(TargetT& target, const AwaitContext& ctx) {
    if constexpr (requires(TargetT& t, const AwaitContext& context) {
        t.on_await_context(context);
    }) {
        target.on_await_context(ctx);
    }
}

constexpr bool sequence_owner_domain_uses_slot(SequenceOwnerDomain domain,
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

constexpr bool sequence_event_uses_slot(IOEventType type,
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
constexpr SequenceOwnerDomain resolve_state_machine_owner_domain(const MachineT& machine) {
    if constexpr (requires {
        { MachineT::kSequenceOwnerDomain } -> std::convertible_to<SequenceOwnerDomain>;
    }) {
        return MachineT::kSequenceOwnerDomain;
    } else if constexpr (requires {
        { MachineT::sequence_owner_domain } -> std::convertible_to<SequenceOwnerDomain>;
    }) {
        return MachineT::sequence_owner_domain;
    } else if constexpr (requires(const MachineT& m) {
        { m.sequence_owner_domain() } -> std::convertible_to<SequenceOwnerDomain>;
    }) {
        return machine.sequence_owner_domain();
    } else {
        return SequenceOwnerDomain::ReadWrite;
    }
}

template <typename ContextT>
constexpr IOEventType custom_awaitable_default_event() {
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
inline constexpr bool is_expected_v = is_expected<std::remove_cvref_t<ResultT>>::value;

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

template <typename ResultT>
auto make_unexpected_io_error(IOError error) -> ResultT
{
    if constexpr (is_expected_v<ResultT>) {
        using ErrorT = typename expected_traits<std::remove_cvref_t<ResultT>>::error_type;
        static_assert(std::is_constructible_v<ErrorT, IOError>,
                      "sequence awaitable error result must be constructible from IOError");
        return std::unexpected(ErrorT(std::move(error)));
    } else {
        static_assert(is_expected_v<ResultT>,
                      "sequence awaitable error paths require std::expected result types");
    }
}

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
    /**
     * @brief 处理 io_uring accept 完成事件
     * @param cqe io_uring 完成队列条目
     * @param handle 句柄
     * @return 该 IO 操作已完成时返回 true，仍需继续处理时返回 false
     */
    bool handle_complete(struct io_uring_cqe* cqe, GHandle handle) override;
#else
    bool handle_complete(GHandle handle) override;  ///< 处理传统后端 accept 就绪事件
#endif

    Host* m_host;  ///< 输出客户端地址；允许为 nullptr
    std::expected<GHandle, IOError> m_result;  ///< 接受连接的结果句柄
};

/**
 * @brief accept 的可等待对象
 * @details `co_await` 后返回新连接句柄，超时或失败时返回 `IOError`。
 */
#if defined(USE_EPOLL) || defined(USE_IOURING)
/**
 * @brief Owner-thread accept，结果在完成时冻结，恢复时不再访问监听资源。
 * @note 仅可在首次 await_suspend 前移动；operation 在最终 frame 位置原地构造。
 *       timeout 只发送 owner 通知，不持有第二份结果或恢复权。
 */
struct AcceptAwaitable : public AwaitableBase {
    AcceptAwaitable(IOController* controller, Host* host) noexcept
        : m_controller(controller), m_host(host) {}
    AcceptAwaitable(AcceptAwaitable&& other) noexcept
        : m_controller(std::exchange(other.m_controller, nullptr)),
          m_host(std::exchange(other.m_host, nullptr)),
          m_duration(other.m_duration), m_timer(std::move(other.m_timer)) {}
    AcceptAwaitable& operator=(AcceptAwaitable&&) = delete;

    bool await_ready() const noexcept { return false; }
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        return suspend(Waker(handle));
    }
    std::expected<GHandle, IOError> await_resume();
    AcceptAwaitable timeout(std::chrono::milliseconds duration) && {
        m_duration = duration;
        return std::move(*this);
    }
    AcceptAwaitable timeout(std::chrono::milliseconds duration) & {
        return std::move(*this).timeout(duration);
    }
    /**
     * @brief 未发布前创建 owner timer；通常由 suspend 惰性调用。
     * @return 无返回值
     */
    void ensure_timer() {
        if (m_duration && !m_timer) {
            m_timer = std::make_shared<AcceptTimeoutTimer>(*m_duration);
        }
    }

    /**
     * @brief 以下入口仅供 IO owner adapter；不得跨线程调用。
     * @param waker 协程唤醒器
     * @return 成功注册挂起操作时返回 true，否则返回 false
     */
    bool suspend(Waker&& waker);
    void timeout_on_owner() noexcept;
    [[nodiscard]] bool select_error(CompletionReason reason, IOError error) noexcept;
    /** @brief epoll 借用 listener；io_uring 接管 accepted fd（含失败/败者的回收）。
     * @param handle 句柄
     * @return 成功接管并处理就绪事件时返回 true，否则返回 false
     *  peer 只写入 typed result，调用方 Host 直到 await_resume 才更新。 */
    [[nodiscard]] bool select_ready(GHandle handle);
    [[nodiscard]] std::expected<ResumeCapability, OperationError> detach();

    std::optional<AcceptOperation> m_operation;
    IOController* m_controller;
#ifdef USE_IOURING
    SqeState* m_registration_state = nullptr; ///< resource arena 的稳定 state，跟随 controller 移动。
#else
    IOController** m_registration_owner = nullptr; ///< reactor 的稳定 owner 槽，跟随 controller 移动。
#endif
    Host* m_host;
    Scheduler* m_scheduler = nullptr;
    std::optional<std::chrono::milliseconds> m_duration;
    std::shared_ptr<AcceptTimeoutTimer> m_timer;
    bool m_timer_attached = false;
    bool m_submitting = false; ///< 时间轮 push 的同步到期只能令 await_suspend 同步继续。
private:
    AcceptAwaitable(const AcceptAwaitable&) = delete;
    AcceptAwaitable& operator=(const AcceptAwaitable&) = delete;
};
#else
struct AcceptAwaitable: public AcceptIOContext, public TimeoutSupport<AcceptAwaitable> {
    AcceptAwaitable(IOController* controller, Host* host)
        : AcceptIOContext(host), m_controller(controller) {}

    bool await_ready() { return false; }
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        return detail::suspend_registered_awaitable<AcceptAwaitable, ACCEPT, kAcceptFailed>(
            *this, handle);
    }
    /**
     * @brief 返回 accept 结果；若失败则返回 IOError
     * @return 成功时返回 GHandle，失败时返回 IOError 错误
     */
    std::expected<GHandle, IOError> await_resume();

    IOController* m_controller;  ///< 关联的 IO 控制器
    Waker m_waker;  ///< 恢复等待协程的唤醒器
};
#endif

// ---- Recv ----

/**
 * @brief recv 操作的上下文
 */
struct RecvIOContext: public IOContextBase {
    RecvIOContext(char* buffer, size_t length)
        : m_buffer(buffer), m_length(length) {}

#ifdef USE_IOURING
    /**
     * @brief 处理 io_uring recv 完成事件
     * @param cqe io_uring 完成队列条目
     * @param handle 句柄
     * @return 该 IO 操作已完成时返回 true，仍需继续处理时返回 false
     */
    bool handle_complete(struct io_uring_cqe* cqe, GHandle handle) override;
#else
    bool handle_complete(GHandle handle) override;  ///< 处理传统后端 recv 就绪事件
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
        return detail::suspend_registered_awaitable<RecvAwaitable, RECV, kRecvFailed>(
            *this, handle);
    }
    /**
     * @brief 返回实际接收字节数；0 可能表示 EOF
     * @return 成功时返回 size_t，失败时返回 IOError 错误
     */
    std::expected<size_t, IOError> await_resume();

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
    /**
     * @brief 处理 io_uring send 完成事件
     * @param cqe io_uring 完成队列条目
     * @param handle 句柄
     * @return 该 IO 操作已完成时返回 true，仍需继续处理时返回 false
     */
    bool handle_complete(struct io_uring_cqe* cqe, GHandle handle) override;
#else
    bool handle_complete(GHandle handle) override;  ///< 处理传统后端 send 就绪事件
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
        return detail::suspend_registered_awaitable<SendAwaitable, SEND, kSendFailed>(
            *this, handle);
    }
    /**
     * @brief 返回实际发送字节数；可能小于请求长度
     * @return 成功时返回 size_t，失败时返回 IOError 错误
     */
    std::expected<size_t, IOError> await_resume();

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
        init_msghdr();
#endif
    }

    template<size_t N>
    ReadvIOContext(std::array<struct iovec, N>& iovecs, size_t count)
        : m_iovecs(iovecs.data(), bounded_borrowed_count(count, N)) {
        mark_invalid_borrowed_count(count, N);
#ifdef USE_IOURING
        init_msghdr();
#endif
    }

    template<size_t N>
    ReadvIOContext(struct iovec (&iovecs)[N], size_t count)
        : m_iovecs(iovecs, bounded_borrowed_count(count, N)) {
        mark_invalid_borrowed_count(count, N);
#ifdef USE_IOURING
        init_msghdr();
#endif
    }

#ifdef USE_IOURING
    /**
     * @brief 处理 io_uring readv 完成事件
     * @param cqe io_uring 完成队列条目
     * @param handle 句柄
     * @return 该 IO 操作已完成时返回 true，仍需继续处理时返回 false
     */
    bool handle_complete(struct io_uring_cqe* cqe, GHandle handle) override;
#else
    bool handle_complete(GHandle handle) override;  ///< 处理传统后端 readv 就绪事件
#endif

    static size_t bounded_borrowed_count(size_t count, size_t capacity) noexcept {
        return count <= capacity ? count : 0;
    }

    static bool borrowed_count_valid(size_t count, size_t capacity) noexcept {
        return count <= capacity;
    }

    void mark_invalid_borrowed_count(size_t count, size_t capacity) {
        if (!borrowed_count_valid(count, capacity)) {
            m_result = std::unexpected(IOError(kParamInvalid, 0));
            m_immediate_result = true;
        }
    }

    std::span<const struct iovec> m_iovecs;  ///< 借用的 iovec 数组视图
    std::expected<size_t, IOError> m_result;  ///< 实际读取字节数或错误
    uint64_t m_immediate_result = 0;  ///< 构造阶段即可返回的参数错误

#ifdef USE_IOURING
    void init_msghdr() {
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

    bool await_ready() { return m_immediate_result; }
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        return detail::suspend_registered_awaitable<ReadvAwaitable, READV, kRecvFailed>(
            *this, handle);
    }
    /**
     * @brief 返回实际读取字节数或错误
     * @return 成功时返回 size_t，失败时返回 IOError 错误
     */
    std::expected<size_t, IOError> await_resume();

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
        init_msghdr();
#endif
    }

    template<size_t N>
    WritevIOContext(std::array<struct iovec, N>& iovecs, size_t count)
        : m_iovecs(iovecs.data(), bounded_borrowed_count(count, N)) {
        mark_invalid_borrowed_count(count, N);
#ifdef USE_IOURING
        init_msghdr();
#endif
    }

    template<size_t N>
    WritevIOContext(struct iovec (&iovecs)[N], size_t count)
        : m_iovecs(iovecs, bounded_borrowed_count(count, N)) {
        mark_invalid_borrowed_count(count, N);
#ifdef USE_IOURING
        init_msghdr();
#endif
    }

#ifdef USE_IOURING
    /**
     * @brief 处理 io_uring writev 完成事件
     * @param cqe io_uring 完成队列条目
     * @param handle 句柄
     * @return 该 IO 操作已完成时返回 true，仍需继续处理时返回 false
     */
    bool handle_complete(struct io_uring_cqe* cqe, GHandle handle) override;
#else
    bool handle_complete(GHandle handle) override;  ///< 处理传统后端 writev 就绪事件
#endif

    static size_t bounded_borrowed_count(size_t count, size_t capacity) noexcept {
        return count <= capacity ? count : 0;
    }

    static bool borrowed_count_valid(size_t count, size_t capacity) noexcept {
        return count <= capacity;
    }

    void mark_invalid_borrowed_count(size_t count, size_t capacity) {
        if (!borrowed_count_valid(count, capacity)) {
            m_result = std::unexpected(IOError(kParamInvalid, 0));
            m_immediate_result = true;
        }
    }

    std::span<const struct iovec> m_iovecs;  ///< 借用的 iovec 数组视图
    std::expected<size_t, IOError> m_result;  ///< 实际写入字节数或错误
    uint64_t m_immediate_result = 0;  ///< 构造阶段即可返回的参数错误

#ifdef USE_IOURING
    void init_msghdr() {
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

    bool await_ready() { return m_immediate_result; }
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        return detail::suspend_registered_awaitable<WritevAwaitable, WRITEV, kSendFailed>(
            *this, handle);
    }
    /**
     * @brief 返回实际写入字节数或错误
     * @return 成功时返回 size_t，失败时返回 IOError 错误
     */
    std::expected<size_t, IOError> await_resume();

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
    /**
     * @brief 处理 io_uring connect 完成事件
     * @param cqe io_uring 完成队列条目
     * @param handle 句柄
     * @return 该 IO 操作已完成时返回 true，仍需继续处理时返回 false
     */
    bool handle_complete(struct io_uring_cqe* cqe, GHandle handle) override;
#else
    bool handle_complete(GHandle handle) override;  ///< 处理传统后端 connect 就绪事件
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
        return detail::suspend_registered_awaitable<ConnectAwaitable, CONNECT, kConnectFailed>(
            *this, handle);
    }
    /**
     * @brief 返回连接结果；失败时返回 IOError
     * @return 成功时返回空值，失败时返回 IOError 错误
     */
    std::expected<void, IOError> await_resume();

    IOController* m_controller;  ///< 关联的 IO 控制器
    Waker m_waker;  ///< 恢复等待协程的唤醒器
};

// ---- Close (直接继承 AwaitableBase，无 IOContext) ----

/**
 * @brief close 的可等待对象
 * @details 关闭请求会立即尝试向当前 IO scheduler 提交，恢复后返回关闭结果。
 * 支持两种持有模式：借用裸控制器（UDP/文件等非共享场景）直接关闭；
 * 或接管共享控制器的引用（AsyncTcpSocket::close 移出 shared_ptr），
 * 仅当自己是最后一个持有者时才真正关闭句柄，否则只释放引用。
 * 对已关闭/为空的控制器的关闭请求返回 IOError(kClosed, 0)。
 */
struct CloseAwaitable: public AwaitableBase, public TimeoutSupport<CloseAwaitable> {
    CloseAwaitable(IOController* controller)
        : m_controller(controller) {}

    CloseAwaitable(const CloseAwaitable&) = delete;
    CloseAwaitable& operator=(const CloseAwaitable&) = delete;
    CloseAwaitable(CloseAwaitable&&) noexcept = default;

    CloseAwaitable& operator=(CloseAwaitable&& other) noexcept {
        if (this != &other) {
            release_owned_ownership();
            TimeoutSupport<CloseAwaitable>::operator=(std::move(other));
            m_owned = std::move(other.m_owned);
            m_controller = other.m_controller;
            m_waker = std::move(other.m_waker);
            m_result = std::move(other.m_result);
        }
        return *this;
    }

    /**
     * @brief 接管共享控制器的引用并按持有计数决定是否真正关闭
     * @param controller 调用方移出的共享控制器（AsyncTcpSocket::close）
     */
    explicit CloseAwaitable(std::shared_ptr<IOController> controller) noexcept
        : m_owned(std::move(controller))
        , m_controller(m_owned.get()) {}

    /**
     * @brief 兜底关闭未执行的共享关闭请求
     * @note close 从未执行（未 await 或提交失败）且本对象是最后持有者时，
     *       直接关闭句柄，避免引用随 awaitable 销毁导致 fd 泄漏。
     */
    ~CloseAwaitable() {
        release_owned_ownership();
    }

    bool await_ready() { return false; }
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        m_waker = Waker(handle);
        if (m_controller == nullptr) {
            // 控制器为空说明句柄已关闭或从未打开。
            m_result = std::unexpected(IOError(kClosed, 0));
            cancel_bound_timeout_timer();
            return false;
        }
        if (m_controller->m_handle == GHandle::invalid()) {
            m_result = std::unexpected(IOError(kClosed, 0));
            cancel_bound_timeout_timer();
            return false;
        }
        auto scheduler = m_waker.get_scheduler();
        if (scheduler == nullptr || scheduler->type() != kIOScheduler) {
            m_result = std::unexpected(IOError(kNotRunningOnIOScheduler, errno));
            // close 在 await_suspend() 中同步提交；在 awaiter 可能销毁前
            // 消费非拥有的超时绑定。
            cancel_bound_timeout_timer();
            return false;
        }
        if (m_owned && m_owned.use_count() > 1) {
            // 仍有其他持有者：仅释放本引用，不真正关闭句柄
            m_owned.reset();
            m_controller = nullptr;
            m_result = {};
            cancel_bound_timeout_timer();
            return false;
        }
        int res = detail::register_io_scheduler_close(scheduler, m_controller);
        if (res == 0) {
            m_result = {};
            // add_close() 返回后 CloseAwaitable 不会挂起，因此不再有 reactor
            // 回调负责取消该 timer。
            cancel_bound_timeout_timer();
            return false;
        }
        m_result = std::unexpected(IOError(kDisconnectError, detail::normalize_awaitable_errno(res)));
        cancel_bound_timeout_timer();
        return false;
    }
    /**
     * @brief 返回关闭结果；失败时返回 IOError
     * @return 成功时返回空值，失败时返回 IOError 错误
     */
    std::expected<void, IOError> await_resume();

    std::shared_ptr<IOController> m_owned;  ///< 共享模式下接管的控制器引用；借用模式为空
    IOController* m_controller;  ///< 关联的 IO 控制器
    Waker m_waker;  ///< 恢复等待协程的唤醒器
    std::expected<void, IOError> m_result;  ///< 关闭操作结果

private:
    void release_owned_ownership() noexcept {
        if (m_owned && m_owned.use_count() == 1 && m_controller != nullptr &&
            m_controller->m_handle != GHandle::invalid()) {
            (void)galay_close(m_controller->m_handle.fd);
            m_controller->m_handle = GHandle::invalid();
        }
        m_owned.reset();
    }
};

// ---- RecvFrom ----

/**
 * @brief recvfrom 操作的上下文
 */
struct RecvFromIOContext: public IOContextBase {
    RecvFromIOContext(char* buffer, size_t length, Host* from)
        : m_buffer(buffer), m_length(length), m_from(from) {}

#ifdef USE_IOURING
    /**
     * @brief 处理 io_uring recvfrom 完成事件
     * @param cqe io_uring 完成队列条目
     * @param handle 句柄
     * @return 该 IO 操作已完成时返回 true，仍需继续处理时返回 false
     */
    bool handle_complete(struct io_uring_cqe* cqe, GHandle handle) override;
#else
    bool handle_complete(GHandle handle) override;  ///< 处理传统后端 recvfrom 就绪事件
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
        return detail::suspend_registered_awaitable<RecvFromAwaitable, RECVFROM, kRecvFailed>(
            *this, handle);
    }
    /**
     * @brief 返回实际接收字节数或错误
     * @return 成功时返回 size_t，失败时返回 IOError 错误
     */
    std::expected<size_t, IOError> await_resume();

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
    /**
     * @brief 处理 io_uring sendto 完成事件
     * @param cqe io_uring 完成队列条目
     * @param handle 句柄
     * @return 该 IO 操作已完成时返回 true，仍需继续处理时返回 false
     */
    bool handle_complete(struct io_uring_cqe* cqe, GHandle handle) override;
#else
    bool handle_complete(GHandle handle) override;  ///< 处理传统后端 sendto 就绪事件
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
        return detail::suspend_registered_awaitable<SendToAwaitable, SENDTO, kSendFailed>(
            *this, handle);
    }
    /**
     * @brief 返回实际发送字节数或错误
     * @return 成功时返回 size_t，失败时返回 IOError 错误
     */
    std::expected<size_t, IOError> await_resume();

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
    /**
     * @brief 处理 io_uring 文件读完成事件
     * @param cqe io_uring 完成队列条目
     * @param handle 句柄
     * @return 该 IO 操作已完成时返回 true，仍需继续处理时返回 false
     */
    bool handle_complete(struct io_uring_cqe* cqe, GHandle handle) override;
#else
    bool handle_complete(GHandle handle) override;  ///< 处理传统后端文件读完成事件
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
        return detail::suspend_registered_awaitable<FileReadAwaitable, FILEREAD, kReadFailed>(
            *this, handle);
    }
    /**
     * @brief 返回实际读取字节数或错误
     * @return 成功时返回 size_t，失败时返回 IOError 错误
     */
    std::expected<size_t, IOError> await_resume();

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
    /**
     * @brief 处理 io_uring 文件写完成事件
     * @param cqe io_uring 完成队列条目
     * @param handle 句柄
     * @return 该 IO 操作已完成时返回 true，仍需继续处理时返回 false
     */
    bool handle_complete(struct io_uring_cqe* cqe, GHandle handle) override;
#else
    bool handle_complete(GHandle handle) override;  ///< 处理传统后端文件写完成事件
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
        return detail::suspend_registered_awaitable<FileWriteAwaitable, FILEWRITE, kWriteFailed>(
            *this, handle);
    }
    /**
     * @brief 返回实际写入字节数或错误
     * @return 成功时返回 size_t，失败时返回 IOError 错误
     */
    std::expected<size_t, IOError> await_resume();

    IOController* m_controller;  ///< 关联的 IO 控制器
    Waker m_waker;  ///< 恢复等待协程的唤醒器
};

// ---- FileWatch ----

/**
 * @brief 文件监控操作的上下文
 */
struct FileWatchIOContext: public IOContextBase {
#ifdef USE_KQUEUE
    FileWatchIOContext(char* buffer,
                       size_t buffer_size,
                       FileWatchEvent events,
                       std::deque<FileWatchResult>* ready_events = nullptr)
        : m_buffer(buffer), m_buffer_size(buffer_size), m_ready_events(ready_events) {
        m_events = static_cast<uint64_t>(events);
    }
#else
    FileWatchIOContext(char* buffer,
                       size_t buffer_size,
                       std::deque<FileWatchResult>* ready_events = nullptr)
        : m_buffer(buffer), m_buffer_size(buffer_size), m_ready_events(ready_events) {}
#endif

#ifdef USE_IOURING
    /**
     * @brief 处理 io_uring 文件监控完成事件
     * @param cqe io_uring 完成队列条目
     * @param handle 句柄
     * @return 该 IO 操作已完成时返回 true，仍需继续处理时返回 false
     */
    bool handle_complete(struct io_uring_cqe* cqe, GHandle handle) override;
#else
    bool handle_complete(GHandle handle) override;  ///< 处理传统后端文件监控事件
#endif

    char* m_buffer;  ///< 监控事件输出缓冲区
    size_t m_buffer_size;  ///< 输出缓冲区容量
    std::deque<FileWatchResult>* m_ready_events = nullptr;  ///< 已 drain 但尚未交付的事件队列
    std::expected<FileWatchResult, IOError> m_result;  ///< 文件监控结果或错误
#ifdef USE_KQUEUE
    uint64_t m_events = 0;  ///< kqueue 模式下的监控事件掩码
#endif
};

/**
 * @brief 文件监控的可等待对象
 */
struct FileWatchAwaitable: public FileWatchIOContext, public TimeoutSupport<FileWatchAwaitable> {
#ifdef USE_KQUEUE
    FileWatchAwaitable(IOController* controller,
                       char* buffer, size_t buffer_size,
                       FileWatchEvent events,
                       std::deque<FileWatchResult>* ready_events = nullptr)
        : FileWatchIOContext(buffer, buffer_size, events, ready_events),
          m_controller(controller) {}
#else
    FileWatchAwaitable(IOController* controller,
                       char* buffer,
                       size_t buffer_size,
                       std::deque<FileWatchResult>* ready_events = nullptr)
        : FileWatchIOContext(buffer, buffer_size, ready_events),
          m_controller(controller) {}
#endif

    bool await_ready() {
        if (m_ready_events != nullptr && !m_ready_events->empty()) {
            m_result = std::move(m_ready_events->front());
            m_ready_events->pop_front();
            return true;
        }
        return false;
    }
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        return detail::suspend_registered_awaitable<FileWatchAwaitable, FILEWATCH, kReadFailed>(
            *this, handle);
    }
    /**
     * @brief 返回文件监控结果或错误
     * @return 成功时返回 FileWatchResult，失败时返回 IOError 错误
     */
    std::expected<FileWatchResult, IOError> await_resume();

    IOController* m_controller;  ///< 关联的 IO 控制器
    Waker m_waker;  ///< 恢复等待协程的唤醒器
};

// ---- SendFile ----

/**
 * @brief sendfile 操作的上下文
 */
struct SendFileIOContext: public IOContextBase {
    SendFileIOContext(int file_fd, off_t offset, size_t count)
        : m_offset(offset), m_count(count), m_transferred(0), m_file_fd(file_fd) {}

#ifdef USE_IOURING
    /**
     * @brief 处理 io_uring sendfile 完成事件
     * @param cqe io_uring 完成队列条目
     * @param handle 句柄
     * @return 该 IO 操作已完成时返回 true，仍需继续处理时返回 false
     */
    bool handle_complete(struct io_uring_cqe* cqe, GHandle handle) override;
#else
    bool handle_complete(GHandle handle) override;  ///< 处理传统后端 sendfile 就绪事件
#endif

    off_t m_offset;  ///< 发送起始偏移
    size_t m_count;  ///< 剩余待发送字节数
    size_t m_transferred;  ///< 当前 awaitable 已累计发送字节数
    std::expected<size_t, IOError> m_result;  ///< 实际发送字节数或错误
    int m_file_fd;  ///< 源文件 fd
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
        return detail::suspend_registered_awaitable<SendFileAwaitable, SENDFILE, kSendFailed>(
            *this, handle);
    }
    /**
     * @brief 返回实际发送字节数或错误
     * @return 成功时返回 size_t，失败时返回 IOError 错误
     */
    std::expected<size_t, IOError> await_resume();

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
    char* read_buffer = nullptr;  ///< read/recv 目标缓冲区
    size_t read_length = 0;  ///< read/recv 请求长度
    const struct iovec* iovecs = nullptr;  ///< readv/writev 的 iovec 指针
    size_t iov_count = 0;  ///< iovec 数量
    const char* write_buffer = nullptr;  ///< send/write 源缓冲区
    size_t write_length = 0;  ///< send/write 请求长度
    Host connect_host{};  ///< connect 目标地址
    std::optional<ResultT> result;  ///< 成功结果
    std::optional<IOError> error;  ///< 失败结果
    MachineSignal signal = MachineSignal::kContinue;  ///< 当前动作类型

    static MachineAction continue_() {
        return MachineAction{};
    }

    static MachineAction wait_read(char* buffer, size_t length) {
        MachineAction action;
        action.signal = MachineSignal::kWaitRead;
        action.read_buffer = buffer;
        action.read_length = length;
        return action;
    }

    static MachineAction wait_write(const char* buffer, size_t length) {
        MachineAction action;
        action.signal = MachineSignal::kWaitWrite;
        action.write_buffer = buffer;
        action.write_length = length;
        return action;
    }

    static MachineAction wait_readv(const struct iovec* iovecs, size_t count) {
        MachineAction action;
        action.signal = MachineSignal::kWaitReadv;
        action.iovecs = iovecs;
        action.iov_count = count;
        return action;
    }

    static MachineAction wait_writev(const struct iovec* iovecs, size_t count) {
        MachineAction action;
        action.signal = MachineSignal::kWaitWritev;
        action.iovecs = iovecs;
        action.iov_count = count;
        return action;
    }

    static MachineAction wait_connect(const Host& host) {
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
        { machine.on_read(std::move(io_result)) } -> std::same_as<void>;
        { machine.on_write(std::move(io_result)) } -> std::same_as<void>;
    };

/**
 * @brief 组合式 sequence awaitable 的抽象基类
 * @details 负责占用 IOController 的读写域、统一挂起/恢复流程以及错误传递。
 */
struct SequenceAwaitableBase: public AwaitableBase, public TimeoutTimerBinding {
    /**
     * @brief sequence 队列中的单个任务条目
     */
    struct IOTask {
        void* task = nullptr;  ///< 具体任务对象指针
        IOContextBase* context = nullptr;  ///< 具体 IO 上下文指针
        IOEventType type;  ///< 默认事件类型
    };

    explicit SequenceAwaitableBase(IOController* controller,
                                   SequenceOwnerDomain requested_domain = SequenceOwnerDomain::ReadWrite)
        : m_controller(controller)
        , m_requested_domain(requested_domain)
        , m_registered_domain(requested_domain) {}

    /**
     * @brief 返回当前队首任务；为空时返回 nullptr
     * @return IOTask* 指针
     */
    virtual IOTask* front() = 0;
    /**
     * @brief 返回当前队首任务的只读视图；为空时返回 nullptr
     * @return const IOTask* 指针
     */
    virtual const IOTask* front() const = 0;
    /**
     * @brief 弹出当前队首任务
     * @return 无返回值
     */
    virtual void pop_front() = 0;
    /**
     * @brief 当前是否没有待执行的 sequence 条目
     * @return 为空时返回 true，否则返回 false
     */
    virtual bool empty() const = 0;

    IOEventType resolve_task_event_type(const IOTask& task) const {
        if (task.context == nullptr) {
            return task.type;
        }
        IOEventType desired = task.context->type();
        return desired == IOEventType::INVALID ? task.type : desired;
    }

    IOEventType active_event_type() const {
        const auto* task = front();
        return task == nullptr ? IOEventType::INVALID : resolve_task_event_type(*task);
    }

    bool waits_on(IOController::Index slot) const {
        return detail::sequence_event_uses_slot(active_event_type(), slot);
    }

    bool claim_requested_domain() {
        if (m_controller == nullptr) {
            return false;
        }

        const bool need_read =
            detail::sequence_owner_domain_uses_slot(m_requested_domain, IOController::READ);
        const bool need_write =
            detail::sequence_owner_domain_uses_slot(m_requested_domain, IOController::WRITE);

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

    void release_registered_domain() {
        if (!m_registered || m_controller == nullptr) {
            m_registered = false;
            return;
        }

        if (detail::sequence_owner_domain_uses_slot(m_registered_domain, IOController::READ) &&
            m_controller->m_sequence_owner[IOController::READ] == this) {
            m_controller->m_sequence_owner[IOController::READ] = nullptr;
        }
        if (detail::sequence_owner_domain_uses_slot(m_registered_domain, IOController::WRITE) &&
            m_controller->m_sequence_owner[IOController::WRITE] == this) {
            m_controller->m_sequence_owner[IOController::WRITE] = nullptr;
        }
#ifdef USE_IOURING
        for (const auto slot : {IOController::READ, IOController::WRITE}) {
            if (m_controller->m_awaitable[slot] == this) {
                m_controller->m_awaitable[slot] = nullptr;
                m_controller->advance_sqe_generation(slot);
            }
        }
#endif
        if (m_controller->m_sequence_owner[IOController::READ] == nullptr &&
            m_controller->m_sequence_owner[IOController::WRITE] == nullptr) {
            m_controller->m_type &= ~SEQUENCE;
            detail::clear_sequence_interest_mask(m_controller);
        } else {
            (void)detail::sync_sequence_interest_mask(m_controller);
        }
        m_registered = false;
    }

    void on_completed() {
        if (std::exchange(m_completed, true)) {
            return;
        }
        // 完成派发时刻先裁决超时竞争，再唤醒协程；否则恢复排队延迟会让
        // 已成功的读被滞后的 TimeoutTimer 误判为超时（见 WithTimeout）。
        // 同步完成路径的 await_resume() 仍可能调用此方法；完成位会让该
        // 兜底调用直接返回。
        cancel_bound_timeout_timer();
        release_registered_domain();
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        return detail::suspend_sequence_awaitable(*this, handle);
    }

#ifdef USE_IOURING
    /**
     * @brief 为 io_uring 准备下一条待提交任务
     * @return SequenceProgress 操作结果
     */
    virtual SequenceProgress prepare_for_submit() = 0;
    /**
     * @brief 处理 io_uring 当前活动任务的完成事件
     * @param cqe io_uring 完成队列条目
     * @param handle 句柄
     * @return SequenceProgress 操作结果
     */
    virtual SequenceProgress on_active_event(struct io_uring_cqe* cqe, GHandle handle) = 0;
#else
    /**
     * @brief 为传统后端准备下一条待执行任务
     * @param handle 句柄
     * @return SequenceProgress 操作结果
     */
    virtual SequenceProgress prepare_for_submit(GHandle handle) = 0;
    virtual SequenceProgress on_active_event(GHandle handle) = 0;  ///< 处理传统后端当前活动任务的就绪事件
#endif

    std::optional<IOError> m_error;  ///< 当前 sequence 错误
    IOController* m_controller;  ///< 关联的 IO 控制器
    Waker m_waker;  ///< 恢复等待协程的唤醒器
    uint64_t m_registered = 0;  ///< 当前是否已登记到 controller
    bool m_completed = false;  ///< 当前 sequence 是否已完成清理
    SequenceOwnerDomain m_requested_domain = SequenceOwnerDomain::ReadWrite;  ///< 期望占用的读写域
    SequenceOwnerDomain m_registered_domain = SequenceOwnerDomain::ReadWrite;  ///< 实际已登记的读写域
};

namespace detail {

inline SequenceInterestMask collect_sequence_interest_mask(const IOController* controller) noexcept {
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
        mask = static_cast<SequenceInterestMask>(mask | sequence_interest_mask(owner->active_event_type()));
        last_owner = owner;
    }
    return mask;
}

inline SequenceInterestMask sync_sequence_interest_mask(IOController* controller) noexcept {
    if (controller == nullptr) {
        return 0;
    }

    controller->m_sequence_interest_mask = collect_sequence_interest_mask(controller);
    return controller->m_sequence_interest_mask;
}

inline void clear_sequence_interest_mask(IOController* controller) noexcept {
    if (controller == nullptr) {
        return;
    }

    controller->m_sequence_interest_mask = 0;
    controller->m_sequence_armed_mask = 0;
}

template <typename Promise>
inline bool suspend_sequence_awaitable(SequenceAwaitableBase& awaitable,
                                     std::coroutine_handle<Promise> handle) {
    awaitable.m_waker = Waker(handle);
    awaitable.m_registered = false;
    awaitable.m_completed = false;
    awaitable.m_error.reset();
#ifdef USE_IOURING
    awaitable.m_sqe_type = SEQUENCE;
#endif

    if (awaitable.m_controller == nullptr) {
        awaitable.m_error = IOError(kClosed, 0);
        return false;
    }

    if (!awaitable.claim_requested_domain()) {
        awaitable.m_error = IOError(kNotReady, 0);
        return false;
    }

#ifdef USE_IOURING
    if (awaitable.prepare_for_submit() == SequenceProgress::kCompleted) {
        return false;
    }
#else
    if (awaitable.prepare_for_submit(awaitable.m_controller->m_handle) == SequenceProgress::kCompleted) {
        return false;
    }
#endif

    (void)sync_sequence_interest_mask(awaitable.m_controller);

    auto* scheduler = awaitable.m_waker.get_scheduler();
    if (scheduler == nullptr || scheduler->type() != kIOScheduler) {
        awaitable.release_registered_domain();
        awaitable.m_error = IOError(kNotRunningOnIOScheduler, errno);
        return false;
    }
    const int ret = register_io_scheduler_event(scheduler, SEQUENCE, awaitable.m_controller);
    if (ret == 1) {
        return false;
    }
    if (ret < 0) {
        awaitable.release_registered_domain();
        awaitable.m_error = IOError(kNotReady, normalize_awaitable_errno(ret));
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
    void queue_many(StepTs&... steps) {
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
        /**
         * @brief 返回步骤关联的 IOContext；本地步骤可返回 nullptr
         * @return IOContextBase* 指针
         */
        virtual IOContextBase* context_base() = 0;
        /**
         * @brief 返回该步骤默认使用的 IO 事件类型
         * @return IOEventType 操作结果
         */
        virtual IOEventType default_event_type() const = 0;
        /**
         * @brief 在真正提交给后端前执行的可选钩子
         * @return 无返回值
         */
        virtual void before_submit() {}
        /**
         * @brief 当前步骤是否为纯本地步骤
         * @return 满足所检查条件时返回 true，否则返回 false
         */
        virtual bool is_local() const = 0;
#ifdef USE_IOURING
        /**
         * @brief 处理 io_uring 事件并返回该步骤是否完成
         * @param owner 所属对象
         * @param cqe io_uring 完成队列条目
         * @param handle 句柄
         * @return 该步骤处理完成时返回 true，否则返回 false
         */
        virtual bool on_event(SequenceAwaitable& owner, struct io_uring_cqe* cqe, GHandle handle) = 0;
#else
        /**
         * @brief 在传统后端提交前尝试同步推进该步骤
         * @param owner 所属对象
         * @param handle 句柄
         * @return 该步骤已完成时返回 true，否则返回 false
         */
        virtual bool on_ready(SequenceAwaitable& owner, GHandle handle) = 0;
        virtual bool on_event(SequenceAwaitable& owner, GHandle handle) = 0;  ///< 处理传统后端就绪事件并返回该步骤是否完成
#endif
    };

    explicit SequenceAwaitable(IOController* controller,
                               SequenceOwnerDomain requested_domain = SequenceOwnerDomain::ReadWrite)
        : SequenceAwaitableBase(controller, requested_domain) {}

    bool await_ready() {
        return m_result_set || m_error.has_value();
    }

    auto await_resume() -> ResultT {
        on_completed();
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
        return detail::make_unexpected_io_error<ResultT>(IOError(kNotReady, errno));
    }

    template <typename StepT>
    StepT& queue(StepT& step) {
        static_assert(std::is_base_of_v<TaskBase, std::remove_cvref_t<StepT>>,
                      "SequenceAwaitable::queue requires a Sequence task");
        (void)emplace_task(step.default_event_type(), step.context_base(), &step);
        return step;
    }

    TaskBase& queue(TaskBase& task) {
        (void)emplace_task(task.default_event_type(), task.context_base(), &task);
        return task;
    }

    template <typename StepT>
    StepT& queue(IOEventType type, StepT& step) {
        static_assert(std::is_base_of_v<TaskBase, std::remove_cvref_t<StepT>>,
                      "SequenceAwaitable::queue requires a Sequence task");
        (void)emplace_task(type, step.context_base(), &step);
        return step;
    }

    TaskBase& queue(IOEventType type, TaskBase& task) {
        (void)emplace_task(type, task.context_base(), &task);
        return task;
    }

    template <typename... StepTs>
    void queue_many(StepTs&... steps) {
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

    bool has_result_value() const {
        return m_result_set && m_result.has_value();
    }

    bool has_failure() const {
        return m_error.has_value();
    }

    std::optional<ResultT> take_result_value() {
        m_result_set = false;
        auto result = std::move(m_result);
        m_result.reset();
        return result;
    }

    std::optional<IOError> take_failure() {
        auto error = std::move(m_error);
        m_error.reset();
        return error;
    }

    void reset_outcome_for_reuse() {
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

    void pop_front() override {
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
    SequenceProgress prepare_for_submit() override {
        while (auto* entry = front()) {
            auto* task = static_cast<TaskBase*>(entry->task);
            if (!task) {
                pop_front();
                continue;
            }
            if (task->is_local()) {
                task->on_event(*this, nullptr, m_controller->m_handle);
                consume_front_if_same(task);
                if (m_result_set) {
                    return SequenceProgress::kCompleted;
                }
                continue;
            }
            task->before_submit();
            entry->context = task->context_base();
            return SequenceProgress::kNeedWait;
        }
        return SequenceProgress::kCompleted;
    }

    SequenceProgress on_active_event(struct io_uring_cqe* cqe, GHandle handle) override {
        auto* entry = front();
        if (!entry) {
            return SequenceProgress::kCompleted;
        }
        auto* task = static_cast<TaskBase*>(entry->task);
        if (!task) {
            pop_front();
            return prepare_for_submit();
        }
        if (task->on_event(*this, cqe, handle)) {
            consume_front_if_same(task);
        }
        if (m_result_set) {
            return SequenceProgress::kCompleted;
        }
        return prepare_for_submit();
    }
#else
    SequenceProgress prepare_for_submit(GHandle handle) override {
        while (auto* entry = front()) {
            auto* task = static_cast<TaskBase*>(entry->task);
            if (!task) {
                pop_front();
                continue;
            }
            task->before_submit();
            entry->context = task->context_base();
            if (task->on_ready(*this, handle)) {
                consume_front_if_same(task);
                if (m_result_set) {
                    return SequenceProgress::kCompleted;
                }
                continue;
            }
            return SequenceProgress::kNeedWait;
        }
        return SequenceProgress::kCompleted;
    }

    SequenceProgress on_active_event(GHandle handle) override {
        auto* entry = front();
        if (!entry) {
            return SequenceProgress::kCompleted;
        }
        auto* task = static_cast<TaskBase*>(entry->task);
        if (!task) {
            pop_front();
            return prepare_for_submit(handle);
        }
        if (task->on_event(*this, handle)) {
            consume_front_if_same(task);
        }
        if (m_result_set) {
            return SequenceProgress::kCompleted;
        }
        return prepare_for_submit(handle);
    }
#endif

private:
    bool emplace_task(IOEventType type, IOContextBase* context, TaskBase* task) {
        if (m_size >= InlineN) {
            fail(IOError(kParamInvalid, 0));
            return false;
        }
        const size_t index = (m_head + m_size) % InlineN;
        m_tasks[index] = IOTask{task, context, type};
        ++m_size;
        return true;
    }

    void consume_front_if_same(TaskBase* task) {
        auto* entry = front();
        if (entry && entry->task == task) {
            pop_front();
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

    /// 恒不挂起：ready 路径不会发生超时注入，`.timeout()` 无需注入通道
    static constexpr bool kAlwaysReady = true;

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
    , public TimeoutMethods<StateMachineAwaitable<MachineT>> {
public:
    using result_type = typename MachineT::result_type;

    StateMachineAwaitable(IOController* controller, MachineT machine)
        : SequenceAwaitableBase(controller, detail::resolve_state_machine_owner_domain(machine))
        , m_recv_context(nullptr, 0)
        , m_readv_context(std::span<const struct iovec>{})
        , m_send_context(nullptr, 0)
        , m_writev_context(std::span<const struct iovec>{})
        , m_connect_context(Host{})
        , m_machine(std::move(machine)) {}

private:
    template <typename ResultT, size_t InlineN, typename FlowT>
    friend class AwaitableBuilder;

    StateMachineAwaitable(IOController* controller,
                          MachineT machine,
                          SequenceOwnerDomain requested_domain)
        : SequenceAwaitableBase(controller, requested_domain)
        , m_recv_context(nullptr, 0)
        , m_readv_context(std::span<const struct iovec>{})
        , m_send_context(nullptr, 0)
        , m_writev_context(std::span<const struct iovec>{})
        , m_connect_context(Host{})
        , m_machine(std::move(machine)) {}

public:
    bool await_ready() {
        return m_result_set || m_error.has_value();
    }

    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        if (!m_context_bound) {
            detail::bind_await_context_if_supported(m_machine, detail::make_await_context(handle));
            m_context_bound = true;
        }
        return SequenceAwaitableBase::await_suspend(handle);
    }

    auto await_resume() -> result_type {
        on_completed();
        if (m_result_set) {
            return std::move(*m_result);
        }
        if (m_error.has_value()) {
            if constexpr (detail::is_expected_v<result_type>) {
                using ErrorT = typename detail::expected_traits<result_type>::error_type;
                if constexpr (std::is_constructible_v<ErrorT, IOError>) {
                    return std::unexpected(ErrorT(*m_error));
                } else {
                    deliver_error_to_machine(std::move(*m_error));
                    m_error.reset();
                    if (m_result_set) {
                        return std::move(*m_result);
                    }
                }
            }
        }
        if constexpr (detail::is_expected_v<result_type>) {
            using ErrorT = typename detail::expected_traits<result_type>::error_type;
            if constexpr (std::is_constructible_v<ErrorT, IOError>) {
                return std::unexpected(ErrorT(IOError(kNotReady, errno)));
            } else {
                deliver_error_to_machine(IOError(kNotReady, errno));
                if (m_result_set) {
                    return std::move(*m_result);
                }
            }
        }
        std::unreachable();
    }

    IOTask* front() override {
        return m_has_active_task ? &m_active_task : nullptr;
    }

    const IOTask* front() const override {
        return m_has_active_task ? &m_active_task : nullptr;
    }

    void pop_front() override {
        clear_active_task();
    }

    bool empty() const override {
        return !m_has_active_task;
    }

    void mark_timeout() {
        const IOError timeout_error(kTimeout, 0);
        const ActiveKind active_kind = m_active_kind;
        clear_active_task();

        switch (active_kind) {
        case ActiveKind::kRead:
        case ActiveKind::kReadv:
            m_machine.on_read(std::unexpected(timeout_error));
            break;
        case ActiveKind::kWrite:
        case ActiveKind::kWritev:
            m_machine.on_write(std::unexpected(timeout_error));
            break;
        case ActiveKind::kConnect:
            deliver_connect(std::unexpected(timeout_error));
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
    SequenceProgress prepare_for_submit() override {
        return pump();
    }

    SequenceProgress on_active_event(struct io_uring_cqe* cqe, GHandle handle) override {
        if (!m_has_active_task) {
            return pump();
        }
        if (m_active_kind == ActiveKind::kRead) {
            if (!m_recv_context.handle_complete(cqe, handle)) {
                return SequenceProgress::kNeedWait;
            }
            auto io_result = std::move(m_recv_context.m_result);
            clear_active_task();
            m_machine.on_read(std::move(io_result));
            return pump();
        }
        if (m_active_kind == ActiveKind::kReadv) {
            if (!m_readv_context.handle_complete(cqe, handle)) {
                return SequenceProgress::kNeedWait;
            }
            auto io_result = std::move(m_readv_context.m_result);
            clear_active_task();
            m_machine.on_read(std::move(io_result));
            return pump();
        }
        if (m_active_kind == ActiveKind::kWrite) {
            if (!m_send_context.handle_complete(cqe, handle)) {
                return SequenceProgress::kNeedWait;
            }
            auto io_result = std::move(m_send_context.m_result);
            clear_active_task();
            m_machine.on_write(std::move(io_result));
            return pump();
        }
        if (m_active_kind == ActiveKind::kWritev) {
            if (!m_writev_context.handle_complete(cqe, handle)) {
                return SequenceProgress::kNeedWait;
            }
            auto io_result = std::move(m_writev_context.m_result);
            clear_active_task();
            m_machine.on_write(std::move(io_result));
            return pump();
        }
        if (m_active_kind == ActiveKind::kConnect) {
            if (!m_connect_context.handle_complete(cqe, handle)) {
                return SequenceProgress::kNeedWait;
            }
            auto io_result = std::move(m_connect_context.m_result);
            clear_active_task();
            deliver_connect(std::move(io_result));
            return pump();
        }
        m_error = IOError(kParamInvalid, 0);
        return SequenceProgress::kCompleted;
    }
#else
    SequenceProgress prepare_for_submit(GHandle handle) override {
        for (size_t i = 0; i < kInlineTransitionCap; ++i) {
            const SequenceProgress progress = pump();
            if (progress == SequenceProgress::kCompleted) {
                return progress;
            }
            if (!m_has_active_task) {
                return SequenceProgress::kCompleted;
            }
            if (m_active_kind == ActiveKind::kRead) {
                if (!m_recv_context.handle_complete(handle)) {
                    return SequenceProgress::kNeedWait;
                }
                auto io_result = std::move(m_recv_context.m_result);
                clear_active_task();
                m_machine.on_read(std::move(io_result));
                continue;
            }
            if (m_active_kind == ActiveKind::kReadv) {
                if (!m_readv_context.handle_complete(handle)) {
                    return SequenceProgress::kNeedWait;
                }
                auto io_result = std::move(m_readv_context.m_result);
                clear_active_task();
                m_machine.on_read(std::move(io_result));
                continue;
            }
            if (m_active_kind == ActiveKind::kWrite) {
                if (!m_send_context.handle_complete(handle)) {
                    return SequenceProgress::kNeedWait;
                }
                auto io_result = std::move(m_send_context.m_result);
                clear_active_task();
                m_machine.on_write(std::move(io_result));
                continue;
            }
            if (m_active_kind == ActiveKind::kWritev) {
                if (!m_writev_context.handle_complete(handle)) {
                    return SequenceProgress::kNeedWait;
                }
                auto io_result = std::move(m_writev_context.m_result);
                clear_active_task();
                m_machine.on_write(std::move(io_result));
                continue;
            }
            if (m_active_kind == ActiveKind::kConnect) {
                if (!m_connect_context.handle_complete(handle)) {
                    return SequenceProgress::kNeedWait;
                }
                auto io_result = std::move(m_connect_context.m_result);
                clear_active_task();
                deliver_connect(std::move(io_result));
                continue;
            }
            m_error = IOError(kParamInvalid, 0);
            return SequenceProgress::kCompleted;
        }
        m_error = IOError(kParamInvalid, 0);
        clear_active_task();
        return SequenceProgress::kCompleted;
    }

    SequenceProgress on_active_event(GHandle handle) override {
        if (!m_has_active_task) {
            return prepare_for_submit(handle);
        }
        if (m_active_kind == ActiveKind::kRead) {
            if (!m_recv_context.handle_complete(handle)) {
                return SequenceProgress::kNeedWait;
            }
            auto io_result = std::move(m_recv_context.m_result);
            clear_active_task();
            m_machine.on_read(std::move(io_result));
            return prepare_for_submit(handle);
        }
        if (m_active_kind == ActiveKind::kReadv) {
            if (!m_readv_context.handle_complete(handle)) {
                return SequenceProgress::kNeedWait;
            }
            auto io_result = std::move(m_readv_context.m_result);
            clear_active_task();
            m_machine.on_read(std::move(io_result));
            return prepare_for_submit(handle);
        }
        if (m_active_kind == ActiveKind::kWrite) {
            if (!m_send_context.handle_complete(handle)) {
                return SequenceProgress::kNeedWait;
            }
            auto io_result = std::move(m_send_context.m_result);
            clear_active_task();
            m_machine.on_write(std::move(io_result));
            return prepare_for_submit(handle);
        }
        if (m_active_kind == ActiveKind::kWritev) {
            if (!m_writev_context.handle_complete(handle)) {
                return SequenceProgress::kNeedWait;
            }
            auto io_result = std::move(m_writev_context.m_result);
            clear_active_task();
            m_machine.on_write(std::move(io_result));
            return prepare_for_submit(handle);
        }
        if (m_active_kind == ActiveKind::kConnect) {
            if (!m_connect_context.handle_complete(handle)) {
                return SequenceProgress::kNeedWait;
            }
            auto io_result = std::move(m_connect_context.m_result);
            clear_active_task();
            deliver_connect(std::move(io_result));
            return prepare_for_submit(handle);
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
                    clear_active_task();
                    return SequenceProgress::kCompleted;
                }
                if (action.read_length == 0) {
                    m_machine.on_read(std::expected<size_t, IOError>(size_t{0}));
                    continue;
                }
                m_recv_context.m_buffer = action.read_buffer;
                m_recv_context.m_length = action.read_length;
                m_active_task = IOTask{nullptr, &m_recv_context, RECV};
                m_has_active_task = true;
                m_active_kind = ActiveKind::kRead;
                return SequenceProgress::kNeedWait;
            case MachineSignal::kWaitReadv:
                if (action.iovecs == nullptr && action.iov_count != 0) {
                    m_error = IOError(kParamInvalid, 0);
                    clear_active_task();
                    return SequenceProgress::kCompleted;
                }
                if (action.iov_count == 0) {
                    m_machine.on_read(std::expected<size_t, IOError>(size_t{0}));
                    continue;
                }
                m_readv_context.m_iovecs = std::span<const struct iovec>(action.iovecs, action.iov_count);
#ifdef USE_IOURING
                m_readv_context.init_msghdr();
#endif
                m_active_task = IOTask{nullptr, &m_readv_context, READV};
                m_has_active_task = true;
                m_active_kind = ActiveKind::kReadv;
                return SequenceProgress::kNeedWait;
            case MachineSignal::kWaitWrite:
                if (action.write_buffer == nullptr && action.write_length != 0) {
                    m_error = IOError(kParamInvalid, 0);
                    clear_active_task();
                    return SequenceProgress::kCompleted;
                }
                if (action.write_length == 0) {
                    m_machine.on_write(std::expected<size_t, IOError>(size_t{0}));
                    continue;
                }
                m_send_context.m_buffer = action.write_buffer;
                m_send_context.m_length = action.write_length;
                m_active_task = IOTask{nullptr, &m_send_context, SEND};
                m_has_active_task = true;
                m_active_kind = ActiveKind::kWrite;
                return SequenceProgress::kNeedWait;
            case MachineSignal::kWaitWritev:
                if (action.iovecs == nullptr && action.iov_count != 0) {
                    m_error = IOError(kParamInvalid, 0);
                    clear_active_task();
                    return SequenceProgress::kCompleted;
                }
                if (action.iov_count == 0) {
                    m_machine.on_write(std::expected<size_t, IOError>(size_t{0}));
                    continue;
                }
                m_writev_context.m_iovecs = std::span<const struct iovec>(action.iovecs, action.iov_count);
#ifdef USE_IOURING
                m_writev_context.init_msghdr();
#endif
                m_active_task = IOTask{nullptr, &m_writev_context, WRITEV};
                m_has_active_task = true;
                m_active_kind = ActiveKind::kWritev;
                return SequenceProgress::kNeedWait;
            case MachineSignal::kWaitConnect:
                m_connect_context.m_host = action.connect_host;
                m_active_task = IOTask{nullptr, &m_connect_context, CONNECT};
                m_has_active_task = true;
                m_active_kind = ActiveKind::kConnect;
                return SequenceProgress::kNeedWait;
            case MachineSignal::kComplete:
                if (!action.result.has_value()) {
                    m_error = IOError(kParamInvalid, 0);
                    clear_active_task();
                    return SequenceProgress::kCompleted;
                }
                m_result = std::move(*action.result);
                m_result_set = true;
                clear_active_task();
                return SequenceProgress::kCompleted;
            case MachineSignal::kFail:
                m_error = action.error.value_or(IOError(kParamInvalid, 0));
                clear_active_task();
                return SequenceProgress::kCompleted;
            }
        }
        m_error = IOError(kParamInvalid, 0);
        clear_active_task();
        return SequenceProgress::kCompleted;
    }

    void deliver_connect(std::expected<void, IOError> result) {
        if constexpr (requires(MachineT& machine, std::expected<void, IOError> connect_result) {
            { machine.on_connect(std::move(connect_result)) } -> std::same_as<void>;
        }) {
            m_machine.on_connect(std::move(result));
        } else {
            if (!result) {
                m_error = result.error();
            } else {
                m_error = IOError(kParamInvalid, 0);
            }
        }
    }

    void deliver_error_to_machine(IOError error) {
        const ActiveKind active_kind = m_active_kind;
        clear_active_task();

        switch (active_kind) {
        case ActiveKind::kRead:
        case ActiveKind::kReadv:
            m_machine.on_read(std::unexpected(std::move(error)));
            break;
        case ActiveKind::kWrite:
        case ActiveKind::kWritev:
            m_machine.on_write(std::unexpected(std::move(error)));
            break;
        case ActiveKind::kConnect:
            deliver_connect(std::unexpected(std::move(error)));
            break;
        case ActiveKind::kNone:
            if (detail::sequence_owner_domain_uses_slot(m_requested_domain, IOController::WRITE) &&
                !detail::sequence_owner_domain_uses_slot(m_requested_domain, IOController::READ)) {
                m_machine.on_write(std::unexpected(std::move(error)));
            } else {
                m_machine.on_read(std::unexpected(std::move(error)));
            }
            break;
        }

        (void)pump();
    }

    void clear_active_task() {
        m_active_task = IOTask{};
        m_has_active_task = false;
        m_active_kind = ActiveKind::kNone;
    }

    RecvIOContext m_recv_context;  ///< 复用的 recv 上下文
    ReadvIOContext m_readv_context;  ///< 复用的 readv 上下文
    SendIOContext m_send_context;  ///< 复用的 send 上下文
    WritevIOContext m_writev_context;  ///< 复用的 writev 上下文
    ConnectIOContext m_connect_context;  ///< 复用的 connect 上下文
    MachineT m_machine;  ///< 用户提供的状态机对象
    IOTask m_active_task{};  ///< 当前已激活的 sequence 任务
    std::optional<result_type> m_result;  ///< 状态机成功结果
    ActiveKind m_active_kind = ActiveKind::kNone;  ///< 当前活动任务类型
    bool m_has_active_task = false;  ///< 当前是否已有活动任务
    bool m_context_bound = false;  ///< 是否已把 AwaitContext 绑定给状态机
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

    IOContextBase* context_base() override {
        return this;
    }

    IOEventType default_event_type() const override {
        return detail::custom_awaitable_default_event<BaseContextT>();
    }

    bool is_local() const override {
        return false;
    }

#ifdef USE_IOURING
    bool on_event(SequenceAwaitable<ResultT, InlineN>& owner, struct io_uring_cqe* cqe, GHandle handle) override {
        if (!BaseContextT::handle_complete(cqe, handle)) {
            return false;
        }
        auto ops = owner.ops();
        (m_owner->*Handler)(ops, static_cast<BaseContextT&>(*this));
        return true;
    }
#else
    bool on_ready(SequenceAwaitable<ResultT, InlineN>& owner, GHandle handle) override {
        if (!BaseContextT::handle_complete(handle)) {
            return false;
        }
        auto ops = owner.ops();
        (m_owner->*Handler)(ops, static_cast<BaseContextT&>(*this));
        return true;
    }

    bool on_event(SequenceAwaitable<ResultT, InlineN>& owner, GHandle handle) override {
        if (!BaseContextT::handle_complete(handle)) {
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

    IOContextBase* context_base() override {
        return nullptr;
    }

    IOEventType default_event_type() const override {
        return IOEventType::INVALID;
    }

    bool is_local() const override {
        return true;
    }

#ifdef USE_IOURING
    bool on_event(SequenceAwaitable<ResultT, InlineN>& owner, struct io_uring_cqe*, GHandle) override {
        auto ops = owner.ops();
        (m_owner->*Handler)(ops);
        return true;
    }
#else
    bool on_ready(SequenceAwaitable<ResultT, InlineN>& owner, GHandle) override {
        auto ops = owner.ops();
        (m_owner->*Handler)(ops);
        return true;
    }

    bool on_event(SequenceAwaitable<ResultT, InlineN>& owner, GHandle) override {
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

    IOContextBase* context_base() override {
        return nullptr;
    }

    IOEventType default_event_type() const override {
        return IOEventType::INVALID;
    }

    bool is_local() const override {
        return true;
    }

#ifdef USE_IOURING
    bool on_event(SequenceAwaitable<ResultT, InlineN>& owner, struct io_uring_cqe*, GHandle) override {
        return run(owner);
    }
#else
    bool on_ready(SequenceAwaitable<ResultT, InlineN>& owner, GHandle) override {
        return run(owner);
    }

    bool on_event(SequenceAwaitable<ResultT, InlineN>& owner, GHandle) override {
        return run(owner);
    }
#endif

private:
    bool run(SequenceAwaitable<ResultT, InlineN>& owner) {
        auto ops = owner.ops();
        const ParseStatus status = (m_owner->*Handler)(ops);
        switch (status) {
            case ParseStatus::kNeedMore:
                if (m_rearm_step == nullptr || m_rearm_step->is_local()) {
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
        NodeKind kind = NodeKind::kLocal;  ///< 节点类型
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
    static Node make_recv_node(char* buffer, size_t length) {
        Node node;
        node.kind = NodeKind::kRecv;
        node.io_handler = &invoke_io<RecvIOContext, Handler>;
        node.read_buffer = buffer;
        node.io_length = length;
        return node;
    }

    template <auto Handler>
    static Node make_send_node(const char* buffer, size_t length) {
        Node node;
        node.kind = NodeKind::kSend;
        node.io_handler = &invoke_io<SendIOContext, Handler>;
        node.write_buffer = buffer;
        node.io_length = length;
        return node;
    }

    template <auto Handler>
    static Node make_readv_node(const struct iovec* iovecs, size_t count) {
        Node node;
        node.kind = NodeKind::kReadv;
        node.io_handler = &invoke_io<ReadvIOContext, Handler>;
        node.iovecs = iovecs;
        node.iov_count = count;
        return node;
    }

    template <auto Handler>
    static Node make_writev_node(const struct iovec* iovecs, size_t count) {
        Node node;
        node.kind = NodeKind::kWritev;
        node.io_handler = &invoke_io<WritevIOContext, Handler>;
        node.iovecs = iovecs;
        node.iov_count = count;
        return node;
    }

    template <auto Handler>
    static Node make_connect_node(const Host& host) {
        Node node;
        node.kind = NodeKind::kConnect;
        node.io_handler = &invoke_io<ConnectIOContext, Handler>;
        node.connect_host = host;
        return node;
    }

    template <auto Handler>
    static Node make_local_node() {
        Node node;
        node.kind = NodeKind::kLocal;
        node.local_handler = &invoke_local<Handler>;
        return node;
    }

    template <auto Handler>
    static Node make_finish_node() {
        Node node;
        node.kind = NodeKind::kFinish;
        node.local_handler = &invoke_local<Handler>;
        return node;
    }

    template <auto Handler>
    static Node make_parse_node(size_t rearm_recv_index) {
        Node node;
        node.kind = NodeKind::kParse;
        node.parse_handler = &invoke_parse<Handler>;
        node.parse_rearm_recv_index = rearm_recv_index;
        return node;
    }

    void on_await_context(const AwaitContext& ctx) {
        if constexpr (requires(FlowT& flow, const AwaitContext& context) {
            flow.on_await_context(context);
        }) {
            if (m_flow != nullptr) {
                m_flow->on_await_context(ctx);
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
            set_io_error(IOError(kNotReady, 0));
            return emit_action_from_outcome();
        }

        const Node& node = m_nodes[m_cursor];
        switch (node.kind) {
        case NodeKind::kRecv:
            m_recv_context.m_buffer = node.read_buffer;
            m_recv_context.m_length = node.io_length;
            m_pending_io = PendingIO::kRead;
            m_pending_index = m_cursor;
            return MachineAction<result_type>::wait_read(node.read_buffer, node.io_length);
        case NodeKind::kReadv:
            m_readv_context.m_iovecs = std::span<const struct iovec>(node.iovecs, node.iov_count);
#ifdef USE_IOURING
            m_readv_context.init_msghdr();
#endif
            m_pending_io = PendingIO::kReadv;
            m_pending_index = m_cursor;
            return MachineAction<result_type>::wait_readv(node.iovecs, node.iov_count);
        case NodeKind::kSend:
            m_send_context.m_buffer = node.write_buffer;
            m_send_context.m_length = node.io_length;
            m_pending_io = PendingIO::kWrite;
            m_pending_index = m_cursor;
            return MachineAction<result_type>::wait_write(node.write_buffer, node.io_length);
        case NodeKind::kWritev:
            m_writev_context.m_iovecs = std::span<const struct iovec>(node.iovecs, node.iov_count);
#ifdef USE_IOURING
            m_writev_context.init_msghdr();
#endif
            m_pending_io = PendingIO::kWritev;
            m_pending_index = m_cursor;
            return MachineAction<result_type>::wait_writev(node.iovecs, node.iov_count);
        case NodeKind::kConnect:
            return run_connect(node);
        case NodeKind::kParse:
            return run_parse(node);
        case NodeKind::kLocal:
        case NodeKind::kFinish:
            return run_local(node);
        }
        set_io_error(IOError(kParamInvalid, 0));
        return emit_action_from_outcome();
    }

    void on_read(std::expected<size_t, IOError> result) {
        if ((m_pending_io != PendingIO::kRead && m_pending_io != PendingIO::kReadv) ||
            m_pending_index >= m_nodes.size()) {
            set_io_error(IOError(kParamInvalid, 0));
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
            invoke_io_node(node, m_recv_context);
        } else {
            m_readv_context.m_result = std::move(result);
            invoke_io_node(node, m_readv_context);
        }
        clear_pending_io();

        if (absorb_ops_outcome()) {
            return;
        }
        if (io_error.has_value()) {
            set_io_error(std::move(*io_error));
            return;
        }
        ++m_cursor;
    }

    void on_write(std::expected<size_t, IOError> result) {
        if ((m_pending_io != PendingIO::kWrite && m_pending_io != PendingIO::kWritev) ||
            m_pending_index >= m_nodes.size()) {
            set_io_error(IOError(kParamInvalid, 0));
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
            invoke_io_node(node, m_send_context);
        } else {
            m_writev_context.m_result = std::move(result);
            invoke_io_node(node, m_writev_context);
        }
        clear_pending_io();

        if (absorb_ops_outcome()) {
            return;
        }
        if (io_error.has_value()) {
            set_io_error(std::move(*io_error));
            return;
        }
        ++m_cursor;
    }

    void on_connect(std::expected<void, IOError> result) {
        if (m_pending_io != PendingIO::kConnect || m_pending_index >= m_nodes.size()) {
            set_io_error(IOError(kParamInvalid, 0));
            return;
        }

        const bool has_value = result.has_value();
        std::optional<IOError> io_error;
        if (!has_value) {
            io_error = result.error();
        }
        m_connect_context.m_result = std::move(result);

        const Node& node = m_nodes[m_pending_index];
        invoke_io_node(node, m_connect_context);
        clear_pending_io();

        if (absorb_ops_outcome()) {
            return;
        }
        if (io_error.has_value()) {
            set_io_error(std::move(*io_error));
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
    static void invoke_io(FlowT* flow, OpsT& ops, IOContextBase& context) {
        (flow->*Handler)(ops, static_cast<ContextT&>(context));
    }

    template <auto Handler>
    static void invoke_local(FlowT* flow, OpsT& ops) {
        (flow->*Handler)(ops);
    }

    template <auto Handler>
    static ParseStatus invoke_parse(FlowT* flow, OpsT& ops) {
        return (flow->*Handler)(ops);
    }

    MachineAction<result_type> run_connect(const Node& node) {
        if (node.io_handler == nullptr) {
            set_io_error(IOError(kParamInvalid, 0));
            return emit_action_from_outcome();
        }

        m_connect_context.m_host = node.connect_host;
        m_pending_io = PendingIO::kConnect;
        m_pending_index = m_cursor;
        return MachineAction<result_type>::wait_connect(node.connect_host);
    }

    MachineAction<result_type> run_local(const Node& node) {
        if (node.local_handler == nullptr) {
            set_io_error(IOError(kParamInvalid, 0));
            return emit_action_from_outcome();
        }

        m_ops_owner.reset_outcome_for_reuse();
        auto ops = m_ops_owner.ops();
        node.local_handler(m_flow, ops);

        if (absorb_ops_outcome()) {
            return emit_action_from_outcome();
        }
        ++m_cursor;
        return MachineAction<result_type>::continue_();
    }

    MachineAction<result_type> run_parse(const Node& node) {
        if (node.parse_handler == nullptr) {
            set_io_error(IOError(kParamInvalid, 0));
            return emit_action_from_outcome();
        }

        m_ops_owner.reset_outcome_for_reuse();
        auto ops = m_ops_owner.ops();
        const ParseStatus status = node.parse_handler(m_flow, ops);

        if (absorb_ops_outcome()) {
            return emit_action_from_outcome();
        }

        switch (status) {
        case ParseStatus::kNeedMore:
            if (node.parse_rearm_recv_index == kInvalidIndex ||
                node.parse_rearm_recv_index >= m_nodes.size() ||
                (m_nodes[node.parse_rearm_recv_index].kind != NodeKind::kRecv &&
                 m_nodes[node.parse_rearm_recv_index].kind != NodeKind::kReadv)) {
                set_io_error(IOError(kParamInvalid, 0));
                return emit_action_from_outcome();
            }
            m_cursor = node.parse_rearm_recv_index;
            return MachineAction<result_type>::continue_();
        case ParseStatus::kContinue:
            return MachineAction<result_type>::continue_();
        case ParseStatus::kCompleted:
            ++m_cursor;
            return MachineAction<result_type>::continue_();
        }
        set_io_error(IOError(kParamInvalid, 0));
        return emit_action_from_outcome();
    }

    void invoke_io_node(const Node& node, IOContextBase& context) {
        if (node.io_handler == nullptr) {
            set_io_error(IOError(kParamInvalid, 0));
            return;
        }
        m_ops_owner.reset_outcome_for_reuse();
        auto ops = m_ops_owner.ops();
        node.io_handler(m_flow, ops, context);
    }

    bool absorb_ops_outcome() {
        if (m_ops_owner.has_result_value()) {
            auto result = m_ops_owner.take_result_value();
            if (result.has_value()) {
                m_result = std::move(*result);
            } else {
                set_io_error(IOError(kParamInvalid, 0));
            }
            return true;
        }
        if (m_ops_owner.has_failure()) {
            auto error = m_ops_owner.take_failure();
            if (error.has_value()) {
                set_io_error(std::move(*error));
            } else {
                set_io_error(IOError(kParamInvalid, 0));
            }
            return true;
        }
        if (!m_ops_owner.empty()) {
            m_ops_owner.clear();
            set_io_error(IOError(kParamInvalid, 0));
            return true;
        }
        return false;
    }

    MachineAction<result_type> emit_action_from_outcome() {
        if (m_result.has_value()) {
            return MachineAction<result_type>::complete(std::move(*m_result));
        }
        if (m_error.has_value()) {
            return MachineAction<result_type>::fail(*m_error);
        }
        return MachineAction<result_type>::continue_();
    }

    void clear_pending_io() {
        m_pending_io = PendingIO::kNone;
        m_pending_index = kInvalidIndex;
    }

    void set_io_error(IOError error) {
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
    SequenceAwaitable<ResultT, InlineN> m_ops_owner;  ///< 复用的 sequence 操作容器
    RecvIOContext m_recv_context;  ///< recv 上下文缓存
    ReadvIOContext m_readv_context;  ///< readv 上下文缓存
    SendIOContext m_send_context;  ///< send 上下文缓存
    WritevIOContext m_writev_context;  ///< writev 上下文缓存
    ConnectIOContext m_connect_context;  ///< connect 上下文缓存

    std::optional<result_type> m_result;  ///< 成功结果
    std::optional<IOError> m_error;  ///< 错误结果
    size_t m_pending_index = kInvalidIndex;  ///< 当前挂起节点索引
    size_t m_cursor = 0;  ///< 当前执行到的节点索引
    PendingIO m_pending_io = PendingIO::kNone;  ///< 当前挂起中的 IO 类型
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
    static auto from_state_machine(IOController* controller, MachineTParam machine) -> StateMachineBuilder<MachineTParam> {
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
        m_nodes.push_back(MachineT::template make_local_node<Handler>());
        return *this;
    }

    template <auto Handler>
    AwaitableBuilder& parse() {
        m_nodes.push_back(MachineT::template make_parse_node<Handler>(m_last_recv_index));
        return *this;
    }

    template <auto Handler>
    AwaitableBuilder& finish() {
        m_nodes.push_back(MachineT::template make_finish_node<Handler>());
        return *this;
    }

    template <auto Handler>
    AwaitableBuilder& recv(char* buffer, size_t length) {
        m_nodes.push_back(MachineT::template make_recv_node<Handler>(buffer, length));
        m_last_recv_index = m_nodes.size() - 1;
        return *this;
    }

    template <auto Handler, size_t N>
    AwaitableBuilder& readv(std::array<struct iovec, N>& iovecs, size_t count = N) {
        if (!ReadvIOContext::borrowed_count_valid(count, N)) {
            m_nodes.push_back(MachineNode{});
            return *this;
        }
        const size_t bounded = ReadvIOContext::bounded_borrowed_count(count, N);
        m_nodes.push_back(MachineT::template make_readv_node<Handler>(iovecs.data(), bounded));
        m_last_recv_index = m_nodes.size() - 1;
        return *this;
    }

    template <auto Handler, size_t N>
    AwaitableBuilder& readv(struct iovec (&iovecs)[N], size_t count = N) {
        if (!ReadvIOContext::borrowed_count_valid(count, N)) {
            m_nodes.push_back(MachineNode{});
            return *this;
        }
        const size_t bounded = ReadvIOContext::bounded_borrowed_count(count, N);
        m_nodes.push_back(MachineT::template make_readv_node<Handler>(iovecs, bounded));
        m_last_recv_index = m_nodes.size() - 1;
        return *this;
    }

    template <auto Handler>
    AwaitableBuilder& send(const char* buffer, size_t length) {
        m_nodes.push_back(MachineT::template make_send_node<Handler>(buffer, length));
        return *this;
    }

    template <auto Handler, size_t N>
    AwaitableBuilder& writev(std::array<struct iovec, N>& iovecs, size_t count = N) {
        if (!WritevIOContext::borrowed_count_valid(count, N)) {
            m_nodes.push_back(MachineNode{});
            return *this;
        }
        const size_t bounded = WritevIOContext::bounded_borrowed_count(count, N);
        m_nodes.push_back(MachineT::template make_writev_node<Handler>(iovecs.data(), bounded));
        return *this;
    }

    template <auto Handler, size_t N>
    AwaitableBuilder& writev(struct iovec (&iovecs)[N], size_t count = N) {
        if (!WritevIOContext::borrowed_count_valid(count, N)) {
            m_nodes.push_back(MachineNode{});
            return *this;
        }
        const size_t bounded = WritevIOContext::bounded_borrowed_count(count, N);
        m_nodes.push_back(MachineT::template make_writev_node<Handler>(iovecs, bounded));
        return *this;
    }

    template <auto Handler>
    AwaitableBuilder& connect(const Host& host) {
        m_nodes.push_back(MachineT::template make_connect_node<Handler>(host));
        return *this;
    }

    auto build() & -> StateMachineAwaitable<MachineT> {
        return build_impl();
    }

    auto build() && -> StateMachineAwaitable<MachineT> {
        return build_impl();
    }

private:
    auto build_impl() -> StateMachineAwaitable<MachineT> {
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
    static auto from_state_machine(IOController* controller, MachineT machine) -> StateMachineBuilder<MachineT> {
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

namespace detail {

/**
 * @brief 一次 await 内完成 TCP 流的完整读/写。
 *
 * `recv()`/`send()` 只保证一次非阻塞系统调用的结果；协议层通常还要在
 * 用户协程里重复挂起，重复挂起会创建 TaskState 和 continuation。这个
 * 状态机把偏移量放进 awaitable 自身，由 SequenceAwaitable 在同一个挂起点
 * 内继续推进，保持零额外子协程分配。
 */
template <bool Write>
struct ExactStreamMachine {
    using result_type = std::expected<size_t, IOError>;
    static constexpr SequenceOwnerDomain kSequenceOwnerDomain =
        Write ? SequenceOwnerDomain::Write : SequenceOwnerDomain::Read;

    char* read_buffer = nullptr;
    const char* write_buffer = nullptr;
    size_t length = 0;
    size_t offset = 0;
    std::optional<IOError> error;

    MachineAction<result_type> advance() {
        if (error.has_value()) {
            return MachineAction<result_type>::fail(*error);
        }
        if (offset == length) {
            return MachineAction<result_type>::complete(offset);
        }
        if constexpr (Write) {
            return MachineAction<result_type>::wait_write(
                write_buffer + offset, length - offset);
        } else {
            return MachineAction<result_type>::wait_read(
                read_buffer + offset, length - offset);
        }
    }

    void on_read(std::expected<size_t, IOError> result) {
        if constexpr (Write) {
            (void)result;
            error = IOError(kNotReady, 0);
            return;
        }
        if (!result) {
            error = result.error();
            return;
        }
        if (result.value() == 0) {
            // EOF before the requested frame is complete is a failed exact read.
            error = IOError(kDisconnectError, 0);
            return;
        }
        offset += result.value();
    }

    void on_write(std::expected<size_t, IOError> result) {
        if constexpr (!Write) {
            (void)result;
            error = IOError(kNotReady, 0);
            return;
        }
        if (!result) {
            error = result.error();
            return;
        }
        if (result.value() == 0) {
            error = IOError(kDisconnectError, 0);
            return;
        }
        offset += result.value();
    }
};

}  // namespace detail


} // namespace galay::kernel

#include "awaitable.inl"

#endif // GALAY_KERNEL_AWAITABLE_H
