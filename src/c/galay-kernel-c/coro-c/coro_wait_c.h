#ifndef GALAY_KERNEL_CORO_WAIT_C_H
#define GALAY_KERNEL_CORO_WAIT_C_H

#include "coro_result_c.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief C coroutine generic wait request handle.
 * @details The handle owns request state. I/O adapters prepare a generation,
 * submit backend work with that generation in user_data, then call complete or
 * cancel. The waiting coroutine reads the final C_IOResult after resume.
 */
typedef struct C_CoroWaitRequest {
    void* request;
} C_CoroWaitRequest;

/**
 * @brief Stable event token for backend user_data.
 * @details I/O adapters may acquire a token after prepare and store the token
 * pointer in an operation object. For raw epoll/kqueue/io_uring user_data,
 * detach the token with galay_coro_wait_event_token_detach_user_data and store
 * the returned pointer. The token retains request state until release, so late
 * backend completions do not dereference a destroyed C request handle. A token
 * must be released exactly once.
 */
typedef struct C_CoroWaitEventToken {
    void* token;
} C_CoroWaitEventToken;

/**
 * @brief Allocate a wait request handle.
 * @param out_request Empty output handle.
 * @return C_IOResultOk on success, C_IOResultInvalid for invalid/non-empty
 * output, C_IOResultError on allocation failure.
 */
C_IOResult galay_coro_wait_request_create(C_CoroWaitRequest* out_request);

/**
 * @brief Destroy a wait request handle.
 * @param request Request handle created by galay_coro_wait_request_create.
 * @return C_IOResultOk on success; C_IOResultInvalid for invalid handles or an
 * actively waiting coroutine.
 */
C_IOResult galay_coro_wait_request_destroy(C_CoroWaitRequest* request);

/**
 * @brief Prepare a request generation before submitting backend work.
 * @param request Request handle.
 * @param out_generation Receives the generation that backend completions must echo.
 * @return C_IOResultOk on success; C_IOResultInvalid if the request is busy.
 */
C_IOResult galay_coro_wait_request_prepare(C_CoroWaitRequest* request,
                                           uint64_t* out_generation);

/**
 * @brief Acquire a stable event token for a prepared request generation.
 * @param request Prepared request handle.
 * @param generation Generation returned by prepare.
 * @param out_token Empty output token. Release it with
 * galay_coro_wait_event_token_release.
 * @return C_IOResultOk on success; C_IOResultInvalid for stale generation,
 * inactive request, invalid handles, or non-empty output token; C_IOResultError
 * on allocation failure.
 */
C_IOResult galay_coro_wait_request_event_token_acquire(C_CoroWaitRequest* request,
                                                       uint64_t generation,
                                                       C_CoroWaitEventToken* out_token);

/**
 * @brief Transfer a token into a raw backend user_data pointer.
 * @param token Token acquired by galay_coro_wait_request_event_token_acquire.
 * The token is emptied on success and must no longer be completed or released
 * through the C_CoroWaitEventToken wrapper.
 * @param out_user_data Empty output pointer that receives the raw token state.
 * Store this pointer in epoll/kqueue/io_uring user_data.
 * @return C_IOResultOk on success; C_IOResultInvalid for invalid, empty, or
 * already detached tokens, or non-empty output pointer.
 */
C_IOResult galay_coro_wait_event_token_detach_user_data(C_CoroWaitEventToken* token,
                                                        void** out_user_data);

/**
 * @brief Suspend the current C coroutine until request completion.
 * @param request Prepared wait request.
 * @param timeout_ms Negative waits indefinitely, zero returns timeout if not
 * already complete, positive waits up to that many milliseconds.
 * @return The result written by complete/cancel/timeout.
 * @note This function must be called from a C coroutine. It suspends the
 * coroutine stack and does not block the scheduler thread.
 */
C_IOResult galay_coro_wait(C_CoroWaitRequest* request, int64_t timeout_ms);

/**
 * @brief Complete a prepared wait request.
 * @param request Request handle.
 * @param generation Generation returned by prepare.
 * @param result Business result to return from galay_coro_wait.
 * @return C_IOResultOk when accepted; C_IOResultInvalid for stale generation or
 * request state other than Pending/Waiting.
 */
C_IOResult galay_coro_wait_request_complete(C_CoroWaitRequest* request,
                                            uint64_t generation,
                                            C_IOResult result);

/**
 * @brief Cancel a prepared wait request.
 * @param request Request handle.
 * @param generation Generation returned by prepare.
 * @return C_IOResultCancelled when accepted; C_IOResultInvalid for stale
 * generation or non-pending request.
 */
C_IOResult galay_coro_wait_request_cancel(C_CoroWaitRequest* request,
                                          uint64_t generation);

/**
 * @brief Complete a request through a backend event token.
 * @param token Token acquired by galay_coro_wait_request_event_token_acquire.
 * @param result Business result to return from galay_coro_wait.
 * @return C_IOResultOk when accepted; C_IOResultInvalid for stale, released, or
 * no-longer-active tokens.
 */
C_IOResult galay_coro_wait_event_token_complete(C_CoroWaitEventToken* token,
                                                C_IOResult result);

/**
 * @brief Cancel a request through a backend event token.
 * @param token Token acquired by galay_coro_wait_request_event_token_acquire.
 * @return C_IOResultCancelled when accepted; C_IOResultInvalid for stale,
 * released, or no-longer-active tokens.
 */
C_IOResult galay_coro_wait_event_token_cancel(C_CoroWaitEventToken* token);

/**
 * @brief Release a backend event token.
 * @param token Token acquired by galay_coro_wait_request_event_token_acquire.
 * @return C_IOResultOk on success; C_IOResultInvalid for invalid or already
 * released tokens.
 */
C_IOResult galay_coro_wait_event_token_release(C_CoroWaitEventToken* token);

/**
 * @brief Complete a request through raw backend user_data.
 * @param user_data Pointer produced by galay_coro_wait_event_token_detach_user_data.
 * @param result Business result to return from galay_coro_wait.
 * @return C_IOResultOk when accepted; C_IOResultInvalid for stale or invalid
 * user_data.
 */
C_IOResult galay_coro_wait_event_user_data_complete(void* user_data,
                                                    C_IOResult result);

/**
 * @brief Cancel a request through raw backend user_data.
 * @param user_data Pointer produced by galay_coro_wait_event_token_detach_user_data.
 * @return C_IOResultCancelled when accepted; C_IOResultInvalid for stale or
 * invalid user_data.
 */
C_IOResult galay_coro_wait_event_user_data_cancel(void* user_data);

/**
 * @brief Release raw backend user_data.
 * @param user_data Pointer produced by galay_coro_wait_event_token_detach_user_data.
 * @return C_IOResultOk on success; C_IOResultInvalid for NULL.
 * @note The pointer is invalid after this call. Call exactly once after the
 * backend operation is retired.
 */
C_IOResult galay_coro_wait_event_user_data_release(void* user_data);

#ifdef __cplusplus
}
#endif

#endif
