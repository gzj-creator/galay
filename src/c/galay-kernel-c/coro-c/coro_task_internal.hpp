#ifndef GALAY_KERNEL_CORO_TASK_INTERNAL_HPP
#define GALAY_KERNEL_CORO_TASK_INTERNAL_HPP

#include "coro_task_c.h"

#include "../../../cpp/galay-kernel/core/waker.h"

namespace galay::kernel::coro_c
{

struct C_CoroTaskInternal;

/**
 * @brief 为 C coroutine task 构造内部 ResumeToken。
 * @details 返回的 token 持有 task 引用，可被 Waker 拷贝并通过 request_resume
 *          将 C coroutine 重新投递到 owner scheduler ready queue。
 */
detail::ResumeToken makeResumeToken(galay_coro_task_t task) noexcept;

C_CoroTaskInternal* currentTask() noexcept;
galay::kernel::Scheduler* currentTaskOwnerScheduler() noexcept;
void retainTask(C_CoroTaskInternal* task) noexcept;
void releaseTask(C_CoroTaskInternal* task) noexcept;
bool prepareCurrentTaskWait() noexcept;
bool rollbackCurrentTaskWait() noexcept;
bool activatePreparedCurrentTaskWait() noexcept;
C_IOResult parkPreparedCurrentTaskWait() noexcept;
bool resumeTaskFromWait(C_CoroTaskInternal* task) noexcept;
bool canResumeTaskFromWaitImmediately(C_CoroTaskInternal* task) noexcept;
/**
 * @brief 在 owner scheduler 当前执行栈上恢复已停泊任务。
 * @note 仅 scheduler 线程且当前不在 C 协程上下文时允许直接切换；其他情况
 *       必须使用 resumeTaskFromWait 的异步入队路径。
 */
bool resumeTaskFromWaitImmediately(C_CoroTaskInternal* task) noexcept;

} // namespace galay::kernel::coro_c

#endif
