#ifndef GALAY_KERNEL_CORO_WAIT_C_H
#define GALAY_KERNEL_CORO_WAIT_C_H

#include "coro_result_c.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief C 协程通用等待 request 句柄。
 * @details 该句柄拥有 request 状态。I/O adapter 会先准备 generation，
 * 再携带该 generation 提交 backend 工作到 user_data，随后调用 complete 或 cancel。
 * 等待中的协程恢复后读取最终 C_IOResult。
 */
typedef struct C_CoroWaitRequest {
    void* request;
} C_CoroWaitRequest;

/**
 * @brief backend user_data 使用的稳定事件 token。
 * @details I/O adapter 可在 prepare 后获取 token，并将 token 指针存入操作对象。
 * 对于原始 epoll/kqueue/io_uring user_data，应通过
 * galay_coro_wait_event_token_detach_user_data 分离 token 并保存返回指针。
 * token 会保留 request 状态直到 release，因此晚到的 backend completion 不会解引用
 * 已销毁的 C request 句柄。每个 token 必须且只能 release 一次。
 */
typedef struct C_CoroWaitEventToken {
    void* token;
} C_CoroWaitEventToken;

/**
 * @brief 分配等待 request 句柄。
 * @param out_request 空输出句柄。
 * @return 成功返回 C_IOResultOk；输出无效或非空返回 C_IOResultInvalid；
 * 分配失败返回 C_IOResultError。
 */
C_IOResult galay_coro_wait_request_create(C_CoroWaitRequest* out_request);

/**
 * @brief 销毁等待 request 句柄。
 * @param request 由 galay_coro_wait_request_create 创建的 request 句柄。
 * @return 成功返回 C_IOResultOk；句柄无效或协程正在等待时返回 C_IOResultInvalid。
 */
C_IOResult galay_coro_wait_request_destroy(C_CoroWaitRequest* request);

/**
 * @brief 提交 backend 工作前准备 request generation。
 * @param request request 句柄。
 * @param out_generation 接收 backend completion 必须回传的 generation。
 * @return 成功返回 C_IOResultOk；request 忙碌时返回 C_IOResultInvalid。
 */
C_IOResult galay_coro_wait_request_prepare(C_CoroWaitRequest* request,
                                           uint64_t* out_generation);

/**
 * @brief 为已准备好的 request generation 获取稳定事件 token。
 * @param request 已准备好的 request 句柄。
 * @param generation prepare 返回的 generation。
 * @param out_token 空输出 token；使用后通过 galay_coro_wait_event_token_release 释放。
 * @return 成功返回 C_IOResultOk；generation 过期、request 未激活、句柄无效或
 * 输出 token 非空返回 C_IOResultInvalid；分配失败返回 C_IOResultError。
 */
C_IOResult galay_coro_wait_request_event_token_acquire(C_CoroWaitRequest* request,
                                                       uint64_t generation,
                                                       C_CoroWaitEventToken* out_token);

/**
 * @brief 将 token 转移为原始 backend user_data 指针。
 * @param token 由 galay_coro_wait_request_event_token_acquire 获取的 token。
 * 成功后 token 会被清空，之后不能再通过 C_CoroWaitEventToken wrapper 完成或释放。
 * @param out_user_data 空输出指针，接收原始 token 状态；该指针应存入
 * epoll/kqueue/io_uring 的 user_data。
 * @return 成功返回 C_IOResultOk；token 无效、为空或已分离，或输出指针非空时
 * 返回 C_IOResultInvalid。
 */
C_IOResult galay_coro_wait_event_token_detach_user_data(C_CoroWaitEventToken* token,
                                                        void** out_user_data);

/**
 * @brief 挂起当前 C 协程直到 request 完成。
 * @param request 已准备好的等待 request。
 * @param timeout_ms 负数表示无限等待，0 表示未完成时立即返回 timeout，
 * 正数表示最多等待对应毫秒数。
 * @return complete/cancel/timeout 写入的结果。
 * @note 该函数必须在 C 协程内调用。它挂起协程栈，不会阻塞 scheduler 线程。
 */
C_IOResult galay_coro_wait(C_CoroWaitRequest* request, int64_t timeout_ms);

/**
 * @brief 完成已准备好的等待 request。
 * @param request request 句柄。
 * @param generation prepare 返回的 generation。
 * @param result 由 galay_coro_wait 返回的业务结果。
 * @return 接受完成时返回 C_IOResultOk；generation 过期或 request 状态不是
 * Pending/Waiting 时返回 C_IOResultInvalid。
 */
C_IOResult galay_coro_wait_request_complete(C_CoroWaitRequest* request,
                                            uint64_t generation,
                                            C_IOResult result);

/**
 * @brief 取消已准备好的等待 request。
 * @param request request 句柄。
 * @param generation prepare 返回的 generation。
 * @return 接受取消时返回 C_IOResultCancelled；generation 过期或 request 非 pending
 * 时返回 C_IOResultInvalid。
 */
C_IOResult galay_coro_wait_request_cancel(C_CoroWaitRequest* request,
                                          uint64_t generation);

/**
 * @brief 通过 backend event token 完成 request。
 * @param token 由 galay_coro_wait_request_event_token_acquire 获取的 token。
 * @param result 由 galay_coro_wait 返回的业务结果。
 * @return 接受完成时返回 C_IOResultOk；token 过期、已释放或不再激活时返回 C_IOResultInvalid。
 */
C_IOResult galay_coro_wait_event_token_complete(C_CoroWaitEventToken* token,
                                                C_IOResult result);

/**
 * @brief 通过 backend event token 取消 request。
 * @param token 由 galay_coro_wait_request_event_token_acquire 获取的 token。
 * @return 接受取消时返回 C_IOResultCancelled；token 过期、已释放或不再激活时
 * 返回 C_IOResultInvalid。
 */
C_IOResult galay_coro_wait_event_token_cancel(C_CoroWaitEventToken* token);

/**
 * @brief 释放 backend event token。
 * @param token 由 galay_coro_wait_request_event_token_acquire 获取的 token。
 * @return 成功返回 C_IOResultOk；token 无效或已释放时返回 C_IOResultInvalid。
 */
C_IOResult galay_coro_wait_event_token_release(C_CoroWaitEventToken* token);

/**
 * @brief 通过原始 backend user_data 完成 request。
 * @param user_data galay_coro_wait_event_token_detach_user_data 生成的指针。
 * @param result 由 galay_coro_wait 返回的业务结果。
 * @return 接受完成时返回 C_IOResultOk；user_data 过期或无效时返回 C_IOResultInvalid。
 */
C_IOResult galay_coro_wait_event_user_data_complete(void* user_data,
                                                    C_IOResult result);

/**
 * @brief 通过原始 backend user_data 取消 request。
 * @param user_data galay_coro_wait_event_token_detach_user_data 生成的指针。
 * @return 接受取消时返回 C_IOResultCancelled；user_data 过期或无效时返回 C_IOResultInvalid。
 */
C_IOResult galay_coro_wait_event_user_data_cancel(void* user_data);

/**
 * @brief 释放原始 backend user_data。
 * @param user_data galay_coro_wait_event_token_detach_user_data 生成的指针。
 * @return 成功返回 C_IOResultOk；NULL 返回 C_IOResultInvalid。
 * @note 调用后该指针失效。backend 操作退役后必须且只能调用一次。
 */
C_IOResult galay_coro_wait_event_user_data_release(void* user_data);

#ifdef __cplusplus
}
#endif

#endif
