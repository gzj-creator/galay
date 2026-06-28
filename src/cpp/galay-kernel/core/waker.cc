/**
 * @file waker.cc
 * @brief Waker 实现
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 实现从 TaskRef 构造 Waker、调度器查找以及重新调度目标协程的唤醒请求。
 */

#include "waker.h"
#include "scheduler.hpp"

namespace galay::kernel {

namespace detail {

ResumeToken::ResumeToken(TaskRef task) noexcept
{
    if (task.isValid()) {
        m_encoded = encode(Kind::CppTask, TaskRefStorageAccess::releaseState(task));
    }
}

ResumeToken ResumeToken::fromCCoroutine(void* state) noexcept
{
    ResumeToken token;
    if (state == nullptr) {
        return token;
    }
    const uintptr_t encoded = encode(Kind::CCoroutine, state);
    if (encoded == 0) {
        return token;
    }
    auto* header = static_cast<ResumeTokenHeader*>(state);
    if (header->hooks == nullptr ||
        header->hooks->request_resume == nullptr ||
        header->hooks->retain == nullptr ||
        header->hooks->release == nullptr) {
        return token;
    }
    header->hooks->retain(state);
    token.m_encoded = encoded;
    return token;
}

ResumeToken::ResumeToken(const ResumeToken& other) noexcept
{
    copyFrom(other);
}

ResumeToken::ResumeToken(ResumeToken&& other) noexcept
    : m_encoded(other.m_encoded)
{
    other.m_encoded = 0;
}

ResumeToken::~ResumeToken()
{
    release();
}

ResumeToken& ResumeToken::operator=(const ResumeToken& other) noexcept
{
    if (this != &other) {
        release();
        copyFrom(other);
    }
    return *this;
}

ResumeToken& ResumeToken::operator=(ResumeToken&& other) noexcept
{
    if (this != &other) {
        release();
        m_encoded = other.m_encoded;
        other.m_encoded = 0;
    }
    return *this;
}

bool ResumeToken::isValid() const noexcept
{
    return m_encoded != 0;
}

Scheduler* ResumeToken::scheduler() const noexcept
{
    if (kind() == Kind::CppTask) {
        auto* task_state = static_cast<TaskState*>(state());
        return task_state != nullptr ? task_state->m_scheduler : nullptr;
    }
    const auto* hooks = coroHooks();
    return hooks != nullptr && hooks->owner_scheduler != nullptr
        ? hooks->owner_scheduler(state())
        : nullptr;
}

bool ResumeToken::requestResume() noexcept
{
    if (kind() == Kind::CppTask) {
        return detail::requestTaskResumeState(static_cast<TaskState*>(state()));
    }
    const auto* hooks = coroHooks();
    return hooks != nullptr && hooks->request_resume != nullptr &&
        hooks->request_resume(state());
}

void ResumeToken::copyFrom(const ResumeToken& other) noexcept
{
    if (other.kind() == Kind::CppTask) {
        auto* task_state = static_cast<TaskState*>(other.state());
        if (task_state != nullptr) {
            TaskRef retained(task_state, true);
            m_encoded = encode(Kind::CppTask, TaskRefStorageAccess::releaseState(retained));
        }
        return;
    }

    if (other.kind() == Kind::CCoroutine && other.state() != nullptr) {
        auto* header = static_cast<ResumeTokenHeader*>(other.state());
        if (header->hooks != nullptr &&
            header->hooks->retain != nullptr &&
            header->hooks->release != nullptr &&
            header->hooks->request_resume != nullptr) {
            header->hooks->retain(other.state());
            m_encoded = encode(Kind::CCoroutine, other.state());
            return;
        }
    }

    m_encoded = 0;
}

void ResumeToken::release() noexcept
{
    if (kind() == Kind::CppTask && state() != nullptr) {
        [[maybe_unused]] TaskRef released =
            TaskRefStorageAccess::adoptState(static_cast<TaskState*>(state()));
    } else if (kind() == Kind::CCoroutine && state() != nullptr) {
        auto* header = static_cast<ResumeTokenHeader*>(state());
        if (header->hooks != nullptr && header->hooks->release != nullptr) {
            header->hooks->release(state());
        }
    }
    m_encoded = 0;
}

void* ResumeToken::state() const noexcept
{
    return reinterpret_cast<void*>(m_encoded & ~kKindMask);
}

ResumeToken::Kind ResumeToken::kind() const noexcept
{
    return static_cast<Kind>(m_encoded & kKindMask);
}

const ResumeTokenHooks* ResumeToken::coroHooks() const noexcept
{
    if (kind() != Kind::CCoroutine || state() == nullptr) {
        return nullptr;
    }
    return static_cast<ResumeTokenHeader*>(state())->hooks;
}

uintptr_t ResumeToken::encode(Kind kind, void* state) noexcept
{
    if (state == nullptr || kind == Kind::Empty) {
        return 0;
    }
    const auto raw = reinterpret_cast<uintptr_t>(state);
    if ((raw & kKindMask) != 0) {
        return 0;
    }
    return (raw & ~kKindMask) | static_cast<uintptr_t>(kind);
}

}  // namespace detail

/**
 * @brief 构造持有给定任务引用的 Waker
 *
 * @param task  此 Waker 将恢复的协程任务
 */
Waker::Waker(TaskRef task) noexcept
    : m_token(detail::ResumeToken(std::move(task)))
{
}

Waker::Waker(detail::ResumeToken token) noexcept
    : m_token(std::move(token))
{
}

/**
 * @brief 查找持有任务关联的调度器
 *
 * @return 所属 Scheduler 指针，若任务未绑定则返回 nullptr
 */
Scheduler* Waker::getScheduler()
{
    return m_token.scheduler();
}

/**
 * @brief 请求在所属调度器上恢复持有任务
 *
 * @details 调用 detail::requestTaskResume，原子地将任务标记为已入队
 * 并提交到调度器。若任务已入队、无效或已完成，请求被静默忽略。
 */
void Waker::wakeUp()
{
    (void)m_token.requestResume();
}

}
