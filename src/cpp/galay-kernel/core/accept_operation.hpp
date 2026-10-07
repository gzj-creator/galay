/** @file accept_operation.hpp
 *  @brief Accept 的结果所有权和 owner-thread timeout 通知；不保存 controller。 */
#ifndef GALAY_KERNEL_ACCEPT_OPERATION_HPP
#define GALAY_KERNEL_ACCEPT_OPERATION_HPP

#include "operation_completion.hpp"
#include "../common/defn.hpp"
#include "../common/error.h"
#include "../common/host.hpp"
#include "../common/timer.hpp"
#include <cerrno>
#include <unistd.h>

namespace galay::kernel {

/** @brief 在 await_resume 移交之前独占 accepted fd，包括未消费结果的回收。 */
class AcceptedConnection final {
public:
    AcceptedConnection(GHandle handle, Host peer) noexcept
        : m_peer(std::move(peer)), m_handle(handle) {}
    AcceptedConnection(AcceptedConnection&& other) noexcept
        : m_peer(std::move(other.m_peer)), m_handle(other.release()) {}
    AcceptedConnection& operator=(AcceptedConnection&&) = delete;
    ~AcceptedConnection() {
        if (m_handle != GHandle::invalid()) {
            // 析构不能返回错误；POSIX close 失败后不重试，以免关闭已复用的 fd。
            const int closed = ::close(m_handle.fd);
            (void)closed;
        }
    }
    [[nodiscard]] GHandle release() noexcept {
        return std::exchange(m_handle, GHandle::invalid());
    }
    [[nodiscard]] const Host& peer() const noexcept { return m_peer; }
private:
    AcceptedConnection(const AcceptedConnection&) = delete;
    AcceptedConnection& operator=(const AcceptedConnection&) = delete;
    Host m_peer;
    GHandle m_handle;
};

using AcceptOperation = OperationCompletion<std::expected<AcceptedConnection, IOError>>;

/**
 * @brief 只在 IO owner 上通知 accept；裁决和恢复权完全属于 AcceptOperation。
 * @note 时间轮可保留本对象至晚到 tick；detach 后不再借用 frame。绑定、触发、
 *       detach 均在同一 owner 上执行，不允许从其他线程直接 handle_timeout。
 */
class AcceptTimeoutTimer final : public Timer {
public:
    explicit AcceptTimeoutTimer(std::chrono::milliseconds duration) : Timer(duration) {}
    AcceptTimeoutTimer(AcceptTimeoutTimer&&) = delete;
    AcceptTimeoutTimer& operator=(AcceptTimeoutTimer&&) = delete;
    void bind(void* operation, void (*notify)(void*) noexcept) noexcept {
        m_operation = operation;
        m_notify = notify;
    }
    void detach() noexcept {
        m_operation = nullptr;
        m_notify = nullptr;
        cancel();
    }
    void handle_timeout() override {
        auto* operation = std::exchange(m_operation, nullptr);
        auto notify = std::exchange(m_notify, nullptr);
        Timer::handle_timeout();
        if (notify) {
            notify(operation); // 可能销毁 frame；之后不访问 operation。
        }
    }
private:
    AcceptTimeoutTimer(const AcceptTimeoutTimer&) = delete;
    AcceptTimeoutTimer& operator=(const AcceptTimeoutTimer&) = delete;
    void* m_operation = nullptr;
    void (*m_notify)(void*) noexcept = nullptr;
};

} // namespace galay::kernel
#endif
