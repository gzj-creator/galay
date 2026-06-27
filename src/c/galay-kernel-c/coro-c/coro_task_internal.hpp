#ifndef GALAY_KERNEL_CORO_TASK_INTERNAL_HPP
#define GALAY_KERNEL_CORO_TASK_INTERNAL_HPP

#include "coro_task_c.h"

#include "../../../cpp/galay-kernel/core/waker.h"

namespace galay::kernel::coro_c
{

/**
 * @brief 为 C coroutine task 构造内部 ResumeToken。
 * @details 返回的 token 持有 task 引用，可被 Waker 拷贝并通过 request_resume
 *          将 C coroutine 重新投递到 owner scheduler ready queue。
 */
detail::ResumeToken makeResumeToken(galay_coro_task_t task) noexcept;

} // namespace galay::kernel::coro_c

#endif
