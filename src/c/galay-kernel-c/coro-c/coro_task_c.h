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
 * @brief C 协程入口函数。
 * @param arg 传给 `galay_coro_spawn` 的用户数据指针。
 * @note 入口函数运行在选定的 owner IO scheduler 线程上，必须正常返回；
 * C++ 异常会转换为 `C_IOResultError`。调用方负责避免进程终止和长时间阻塞工作。
 */
typedef void (*galay_coro_entry_fn)(void* arg);

/**
 * @brief C 协程创建选项。
 */
typedef struct C_CoroOptions {
    size_t stack_size;  ///< 可用栈字节数；0 表示使用默认值。
} C_CoroOptions;

/**
 * @brief 不透明 C 协程任务句柄。
 */
typedef struct galay_coro_task {
    void* task;
} galay_coro_task_t;

/**
 * @brief 返回默认 C 协程选项。
 * @return 默认栈大小和 scheduler 设置。
 */
C_CoroOptions galay_coro_options_default(void);

/**
 * @brief 在运行中的 galay runtime 上创建有栈 C 协程。
 * @param runtime 已运行的 C runtime 句柄；runtime 生命周期必须长于任务。
 * @param entry 协程入口函数，不能为 NULL。
 * @param arg 传给 `entry` 的用户数据。
 * @param options 可选创建选项；NULL 表示使用默认值。
 * @param out_task 输出任务句柄；调用方最终必须销毁它。
 * @return 成功返回 `C_IOResultOk`；runtime/参数无效或已停止返回 `C_IOResultInvalid`；
 * 分配失败或平台不支持返回 `C_IOResultError`。
 * @note 任务通过 runtime 的 IO scheduler 和内部 ReadyEntry 边界调度。
 * 该 API 不会创建 C++ Task wrapper。
 */
C_IOResult galay_coro_spawn(galay_kernel_runtime_t* runtime,
                            galay_coro_entry_fn entry,
                            void* arg,
                            const C_CoroOptions* options,
                            galay_coro_task_t* out_task);

/**
 * @brief 让出当前 C 协程，并将其重新入队到 owner scheduler。
 * @return 协程恢复后返回 `C_IOResultOk`；不在 C 协程内调用返回 `C_IOResultInvalid`；
 * 重新入队失败返回 `C_IOResultError`。
 * @note 该函数挂起协程栈，不会阻塞调用线程。
 */
C_IOResult galay_coro_yield(void);

/**
 * @brief 获取当前正在运行的 C 协程。
 * @param out_task 输出当前协程的拥有型句柄；进入时必须为空，完成后需用
 * `galay_coro_destroy` 释放。
 * @return 在 C 协程内调用返回 `C_IOResultOk`；否则返回 `C_IOResultInvalid`。
 */
C_IOResult galay_coro_current(galay_coro_task_t* out_task);

/**
 * @brief 等待 C 协程进入完成或取消状态。
 * @param task `galay_coro_spawn` 返回的任务句柄。
 * @param timeout_ms 负数表示无限等待，0 表示轮询，正数表示最多等待对应毫秒数。
 * @return 任务结果、`C_IOResultCancelled`、`C_IOResultTimeout` 或 `C_IOResultInvalid`。
 * @note `join` 只等待状态，不会直接恢复协程。从 C 协程或任意 galay scheduler 线程
 * 调用会返回 `C_IOResultInvalid`；scheduler 代码内应使用基于 request 的协程等待。
 */
C_IOResult galay_coro_join(galay_coro_task_t* task, int64_t timeout_ms);

/**
 * @brief 协作式取消未运行中的 C 协程。
 * @param task `galay_coro_spawn` 返回的任务句柄。
 * @return 成功记录取消时返回 `C_IOResultCancelled`；任务已完成时返回原始任务结果；
 * 句柄无效或任务正在运行时返回 `C_IOResultInvalid`。
 */
C_IOResult galay_coro_cancel(galay_coro_task_t* task);

/**
 * @brief 释放 C 协程任务句柄。
 * @param task 任务句柄；成功时 `task->task` 会被置为 NULL。
 * @return 成功返回 `C_IOResultOk`；任务无效或尚未终止返回 `C_IOResultInvalid`。
 * @note 销毁 owning runtime 前必须先销毁所有 C 协程任务。销毁已入队任务会标记
 * handle 引用已释放；任务存储会在 scheduler 持有的 ready 引用释放后回收。
 */
C_IOResult galay_coro_destroy(galay_coro_task_t* task);

#ifdef __cplusplus
}
#endif

#endif
