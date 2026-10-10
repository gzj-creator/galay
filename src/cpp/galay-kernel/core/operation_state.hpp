/**
 * @file operation_state.hpp
 * @brief 与资源、结果类型、协程和后端解耦的 owner-thread 状态机。
 */
#ifndef GALAY_KERNEL_OPERATION_STATE_HPP
#define GALAY_KERNEL_OPERATION_STATE_HPP

#include "operation_key.hpp"
#include <expected>
#include <optional>

namespace galay::kernel {

/** @brief Owner 可观察的阶段；逻辑完成发布期间不调用任何外部回调。 */
enum class OperationPhase : uint8_t {
    kCreated,
    kSubmitted,
    kCancelRequested,
    kDraining,
    kSafeToResume,
    kResumeIssued,
};

/** @brief 逻辑完成原因；请求 cancel 本身不选择结果。 */
enum class CompletionReason : uint8_t {
    kReady,
    kBackendError,
    kTimedOut,
    kCancelled,
    kResourceClosed,
    kRuntimeStopped,
};

/** @brief 调用者必须处理的 attachment/result/resume 协议错误。 */
enum class OperationError : uint8_t {
    kAlreadyCompleted,
    kReferenceOverflow,
    kNoPhysicalReference,
    kNotSafeToResume,
    kResumeAlreadyTaken,
    kResultUnavailable,
    kDrainIncomplete,  // FIX: physical refs 尚未完全释放
};

/**
 * @brief 唯一完成裁决与 physical-drain 记账，不包含结果或恢复句柄。
 *
 * 所有读写只允许同一 owner executor 串行执行。跨线程 cancel/CQE 必须
 * 先经 owner command queue 发布，不能直接调用本对象；因此无需原子或锁。
 * physical refs 只是计数，不自动保活对象：adapter 必须持有实际 storage
 * owner，先 retain 再发布请求，且最后一个回执处理完后才 release。
 *
 * 地址从构造至最后一次完成/消费保持稳定，禁止 copy/move。若 awaiter
 * 在挂起前需要移动，应在最终存储位置构造状态，而非移动已发布 attachment。
 * 无 backend 的同步完成可使用无效 key；mark_submitted() 要求有效 key。
 * 析构不是取消 API，也不会等待 backend、唤醒任务或回收 native resource。
 */
class OperationState final {
public:
    explicit OperationState(OperationKey key = {}) noexcept : m_key(key) {}
    OperationState(OperationState&&) = delete;
    OperationState& operator=(OperationState&&) = delete;

    [[nodiscard]] OperationKey key() const noexcept { return m_key; }
    [[nodiscard]] OperationPhase phase() const noexcept { return m_phase; }
    [[nodiscard]] uint32_t physical_reference_count() const noexcept { return m_physical_refs; }

    /**
     * @brief 未选出 winner 时返回 nullopt，不暴露虚假的默认完成原因。
     * @return 已裁决的完成原因；尚未完成时为 std::nullopt
     */
    [[nodiscard]] std::optional<CompletionReason> completion_reason() const noexcept {
        return has_completed() ? std::optional(m_reason) : std::nullopt;
    }

    /**
     * @brief 仅首次有效 key 的 Created -> Submitted 成功；失败不改变状态。
     * @return 成功从 Created 转为 Submitted 时返回 true，否则返回 false
     */
    [[nodiscard]] bool mark_submitted() noexcept {
        if (m_phase != OperationPhase::kCreated || !m_key.is_valid()) {
            return false;
        }
        m_phase = OperationPhase::kSubmitted;
        return true;
    }

    /**
     * @brief 仅记录首次取消请求；完成裁决仍由 try_complete() 执行。
     * @return 首次成功记录取消请求时返回 true，否则返回 false
     */
    [[nodiscard]] bool request_cancel() noexcept {
        if (m_phase != OperationPhase::kCreated && m_phase != OperationPhase::kSubmitted) {
            return false;
        }
        m_phase = OperationPhase::kCancelRequested;
        return true;
    }

    /**
     * @brief 发布 backend/timeout attachment 前增加引用。
     * @return 已有逻辑 winner 或计数溢出时返回 typed error，不改变计数。
     * @note cancel SQE 等 drain 所需 attachment 必须在选出 winner 前登记；
     *       不允许在 SafeToResume 之后重新进入 Draining。
     */
    [[nodiscard]] std::expected<void, OperationError> add_physical_reference() noexcept {
        if (has_completed()) {
            return std::unexpected(OperationError::kAlreadyCompleted);
        }
        if (m_physical_refs == UINT32_MAX) {
            return std::unexpected(OperationError::kReferenceOverflow);
        }
        ++m_physical_refs;
        return {};
    }

    /**
     * @brief attachment 不再可能访问 storage 后释放其引用。
     * @return true 仅表示本次从 Draining 进入 SafeToResume；false 表示仍需
     *         完成或 drain。无引用可释放时返回错误，绝不下溢。
     */
    [[nodiscard]] std::expected<bool, OperationError> release_physical_reference() noexcept {
        if (m_physical_refs == 0) {
            return std::unexpected(OperationError::kNoPhysicalReference);
        }
        --m_physical_refs;
        if (m_physical_refs == 0 && m_phase == OperationPhase::kDraining) {
            m_phase = OperationPhase::kSafeToResume;
            return true;
        }
        return false;
    }

    /**
     * @brief 首个候选获胜；败者不得再修改结果、调用回调或唤醒任务。
     * @param reason 完成原因
     * @return 成功取得完成裁决时返回 true，已有其他完成者时返回 false
     */
    [[nodiscard]] bool try_complete(CompletionReason reason) noexcept {
        if (has_completed()) {
            return false;
        }
        m_reason = reason;
        m_phase = m_physical_refs == 0 ? OperationPhase::kSafeToResume
                                       : OperationPhase::kDraining;
        return true;
    }

    /**
     * @brief SafeToResume 后只移交一次恢复权；这不是 storage 已析构的标记。
     * @return 成功移交一次恢复权时返回 true，否则返回 false
     */
    [[nodiscard]] bool mark_resume_issued() noexcept {
        if (m_phase != OperationPhase::kSafeToResume) {
            return false;
        }
        m_phase = OperationPhase::kResumeIssued;
        return true;
    }

private:
    OperationState(const OperationState&) = delete;
    OperationState& operator=(const OperationState&) = delete;

    [[nodiscard]] bool has_completed() const noexcept {
        return m_phase == OperationPhase::kDraining ||
               m_phase == OperationPhase::kSafeToResume ||
               m_phase == OperationPhase::kResumeIssued;
    }

    OperationKey m_key;
    uint32_t m_physical_refs = 0;
    OperationPhase m_phase = OperationPhase::kCreated;
    CompletionReason m_reason = CompletionReason::kBackendError; // 完成前不可观察。
};

} // namespace galay::kernel
#endif // GALAY_KERNEL_OPERATION_STATE_HPP
