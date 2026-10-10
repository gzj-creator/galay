/**
 * @file operation_completion.hpp
 * @brief 在独立状态机之上拥有 typed result 与唯一 resume capability。
 */
#ifndef GALAY_KERNEL_OPERATION_COMPLETION_HPP
#define GALAY_KERNEL_OPERATION_COMPLETION_HPP

#include "operation_state.hpp"
#include "waker.h"
#include <type_traits>
#include <utility>

namespace galay::kernel {

/**
 * @brief Operation 内部的 move-only 恢复权；复用现有 Waker 队列入口。
 * @note resume() 先清空自身，再调用可能内联销毁 operation 的 Waker；
 *       重复消费同一 capability 是空操作，不能复制出第二份恢复权。
 */
class ResumeCapability final {
public:
    explicit ResumeCapability(Waker&& resume = {}) noexcept : m_resume(std::move(resume)) {}
    ResumeCapability(ResumeCapability&&) noexcept = default;
    ResumeCapability& operator=(ResumeCapability&&) noexcept = default;

    /**
     * @brief 以移动方式消费恢复权；owner scheduler 必须仍在运行。
     * @return 无返回值
     */
    void resume() && noexcept {
        Waker resume = std::move(m_resume);
        resume.wake_up(); // 此后不再访问 this；局部 Waker 独立于 operation 存活。
    }

private:
    ResumeCapability(const ResumeCapability&) = delete;
    ResumeCapability& operator=(const ResumeCapability&) = delete;
    Waker m_resume;
};

/**
 * @brief 一次异步操作的结果/恢复权存储，供 owner-side adapter 组合使用。
 * @tparam Result noexcept 可移动的结果类型，通常为 std::expected<T, IOError>；
 *         无返回值操作使用 std::expected<void, IOError>，而非裸 void。
 *
 * 继承 OperationState 的单 owner、固定地址、显式 physical-drain 契约。
 * 不依赖 IOController、TimeoutTimer 或 reactor；这些 adapter 不得另外
 * 存储/覆盖结果。payload 的移动/析构不得重入本对象。内部不分配内存，
 * 不隐式 wake；调用者先 detach，再 take_resume()，最后消费局部恢复权。
 * 唤醒可能内联销毁本对象，故调用者之后不得访问 operation/frame/resource。
 *
 * 未使用的 accepted fd 等 native resource 必须由 Result 的 RAII 或 adapter
 * 回收；本模板不会凭空获得整数 fd 的 ownership。败者的参数保持原状。
 * 当前模块不负责注册表、timer 仲裁或 Runtime drain，不代表 accept 已迁移。
 */
template <typename Result>
requires std::is_nothrow_move_constructible_v<Result>
class OperationCompletion final {
public:
    /**
     * @param resume 必须显式移动恢复权，禁止从 Waker 左值隐式复制；同步完成可留空。
     * @param key 键
     */
    explicit OperationCompletion(OperationKey key = {}, Waker&& resume = {}) noexcept
        : m_resume(std::move(resume)), m_state(key) {}

    OperationCompletion(OperationCompletion&&) = delete;
    OperationCompletion& operator=(OperationCompletion&&) = delete;

    /**
     * @brief 只读诊断视图；调用者不能绕过 typed result 的完成入口。
     * @return const OperationState& 只读引用
     */
    [[nodiscard]] const OperationState& state() const noexcept { return m_state; }
    [[nodiscard]] bool mark_submitted() noexcept { return m_state.mark_submitted(); }
    [[nodiscard]] bool request_cancel() noexcept { return m_state.request_cancel(); }

    /**
     * @brief 转发 attachment 发布前的 retain；失败时不得发布该请求。
     * @return 成功时返回空值，失败时返回 OperationError 错误
     */
    [[nodiscard]] std::expected<void, OperationError> add_physical_reference() noexcept {
        return m_state.add_physical_reference();
    }

    /**
     * @brief 转发 attachment drain；true 后调用者可以 take_resume()。
     * @return 成功时返回 bool，失败时返回 OperationError 错误
     */
    [[nodiscard]] std::expected<bool, OperationError> release_physical_reference() noexcept {
        return m_state.release_physical_reference();
    }

    /**
     * @brief 先选 winner，再移动结果；失败时完全不消费 result。
     * @param reason 完成原因
     * @param result 结果对象
     * @return 成功取得完成裁决时返回 true，已有其他完成者时返回 false
     * @note 不调用外部代码或唤醒器（Result 的 noexcept move 除外）。
     */
    [[nodiscard]] bool try_complete(CompletionReason reason, Result&& result) noexcept {
        if (!m_state.try_complete(reason)) {
            return false;
        }
        // emplace 返回内部结果的可写别名；本协议刻意不向 adapter 暴露它。
        (void)m_result.emplace(std::move(result));
        return true;
    }

    /**
     * @brief 安全完成后一次性移交 move-only 恢复权，不执行唤醒。
     * @return drain 未完成或恢复权已取出时返回对应错误；可重试前一种错误。
     */
    [[nodiscard]] std::expected<ResumeCapability, OperationError> take_resume() noexcept {
        if (m_state.phase() == OperationPhase::kResumeIssued) {
            return std::unexpected(OperationError::kResumeAlreadyTaken);
        }
        if (!m_state.mark_resume_issued()) {
            return std::unexpected(OperationError::kNotSafeToResume);
        }
        return ResumeCapability(std::move(m_resume));
    }

    /**
     * @brief 恢复权移交后由 await_resume 一次性取走结果。
     * @return 未完成/draining/未移交恢复权或重复取结果时返回 kResultUnavailable。
     */
    [[nodiscard]] std::expected<Result, OperationError> take_result() noexcept {
        if (m_state.phase() != OperationPhase::kResumeIssued) {
            return std::unexpected(OperationError::kResultUnavailable);
        }

        // FIX: 必须等待所有 physical refs drain 完成
        if (m_state.physical_reference_count() != 0) {
            // 仍有 backend attachment 未清理，不能安全地取结果
            return std::unexpected(OperationError::kDrainIncomplete);
        }

        if (!m_result) {
            return std::unexpected(OperationError::kResultUnavailable);
        }

        std::expected<Result, OperationError> result(std::in_place, std::move(*m_result));
        m_result.reset();
        return result;
    }

private:
    OperationCompletion(const OperationCompletion&) = delete;
    OperationCompletion& operator=(const OperationCompletion&) = delete;

    std::optional<Result> m_result; // 构造后无结果；只由 winner 写入。
    Waker m_resume;
    OperationState m_state;
};

} // namespace galay::kernel
#endif // GALAY_KERNEL_OPERATION_COMPLETION_HPP
