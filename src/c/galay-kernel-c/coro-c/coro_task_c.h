#ifndef GALAY_KERNEL_CORO_TASK_C_H
#define GALAY_KERNEL_CORO_TASK_C_H

#include "coro_result_c.h"
#include "../core-c/runtime_c.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief C coroutine entry function.
 * @param arg User data pointer passed to `galay_coro_spawn`.
 * @note The entry runs on the selected owner IO scheduler thread. It must return
 * normally; C++ exceptions are converted to `C_IOResultError`. Process
 * termination and long blocking work are the caller's responsibility to avoid.
 */
typedef void (*galay_coro_entry_fn)(void* arg);

/**
 * @brief C coroutine spawn options.
 */
typedef struct C_CoroOptions {
    size_t stack_size;  ///< Usable stack bytes. 0 selects the default.
} C_CoroOptions;

/**
 * @brief Opaque C coroutine task handle.
 */
typedef struct galay_coro_task {
    void* task;
} galay_coro_task_t;

/**
 * @brief Return default C coroutine options.
 * @return Default stack size and scheduler settings.
 */
C_CoroOptions galay_coro_options_default(void);

/**
 * @brief Spawn a stackful C coroutine on a running galay runtime.
 * @param runtime Running C runtime handle. The runtime must outlive the task.
 * @param entry Coroutine entry function. Must not be NULL.
 * @param arg User data passed to `entry`.
 * @param options Optional spawn options; NULL selects defaults.
 * @param out_task Output task handle. The caller must eventually destroy it.
 * @return `C_IOResultOk` on success; `C_IOResultInvalid` for invalid or stopped
 * runtime / parameters; `C_IOResultError` for allocation or unsupported platform.
 * @note The task is scheduled through the runtime's IO scheduler using the
 * internal ReadyEntry boundary. The API does not spawn a C++ Task wrapper.
 */
C_IOResult galay_coro_spawn(galay_kernel_runtime_t* runtime,
                            galay_coro_entry_fn entry,
                            void* arg,
                            const C_CoroOptions* options,
                            galay_coro_task_t* out_task);

/**
 * @brief Yield the current C coroutine and requeue it on its owner scheduler.
 * @return `C_IOResultOk` after the coroutine is resumed; `C_IOResultInvalid`
 * outside a C coroutine; `C_IOResultError` if requeue fails.
 * @note This suspends the coroutine stack, not the calling OS thread.
 */
C_IOResult galay_coro_yield(void);

/**
 * @brief Get the currently running C coroutine.
 * @param out_task Output owning handle to the current coroutine. It must be
 * empty on entry and later released with `galay_coro_destroy` after completion.
 * @return `C_IOResultOk` inside a C coroutine; `C_IOResultInvalid` otherwise.
 */
C_IOResult galay_coro_current(galay_coro_task_t* out_task);

/**
 * @brief Wait until a C coroutine reaches done or cancelled state.
 * @param task Task handle returned by `galay_coro_spawn`.
 * @param timeout_ms Negative waits indefinitely, zero polls, positive waits up
 * to that many milliseconds.
 * @return Task result, `C_IOResultCancelled`, `C_IOResultTimeout`, or
 * `C_IOResultInvalid`.
 * @note `join` only waits on state; it never resumes a coroutine directly.
 * Calling it from a C coroutine or from any galay scheduler thread returns
 * `C_IOResultInvalid` until Task 5 adds cooperative wait suspension.
 */
C_IOResult galay_coro_join(galay_coro_task_t* task, int64_t timeout_ms);

/**
 * @brief Cooperatively cancel a non-running C coroutine.
 * @param task Task handle returned by `galay_coro_spawn`.
 * @return `C_IOResultCancelled` when cancellation is recorded;
 * the original task result when already done; `C_IOResultInvalid` for invalid
 * handles or a currently running task.
 */
C_IOResult galay_coro_cancel(galay_coro_task_t* task);

/**
 * @brief Release a C coroutine task handle.
 * @param task Task handle. On success `task->task` is set to NULL.
 * @return `C_IOResultOk` on success, `C_IOResultInvalid` for invalid or
 * not-yet-terminal task.
 * @note Destroy all C coroutine tasks before destroying the owning runtime.
 * Destroying a queued task marks the handle reference released; the task storage
 * is reclaimed after any scheduler-held ready reference is released.
 */
C_IOResult galay_coro_destroy(galay_coro_task_t* task);

#ifdef __cplusplus
}
#endif

#endif
