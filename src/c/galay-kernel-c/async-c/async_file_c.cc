#include "async_file_c.h"

#include "../../../cpp/galay-kernel/async/async_file.h"
#include "../coro-c/coro_task_internal.hpp"
#include "../coro-c/coro_wait_c.h"
#include <galay/c/galay-bridge-c/coro-c/c_coro_async_file_bridge.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <limits>
#include <new>
#include <string>
#include <sys/types.h>
#include <utility>

namespace
{

C_IOResult make_result(C_IOResultCode code, int sys_errno = 0)
{
    return C_IOResult{code, sys_errno, 0, 0, nullptr};
}

bool offset_fits_off_t(int64_t offset)
{
    return offset >= 0 && offset <= static_cast<int64_t>(std::numeric_limits<off_t>::max());
}

#if defined(USE_KQUEUE) || defined(USE_IOURING)

bool is_valid_open_mode(C_AsyncFileOpenMode mode)
{
    switch (mode) {
    case C_AsyncFileOpenModeRead:
    case C_AsyncFileOpenModeWrite:
    case C_AsyncFileOpenModeReadWrite:
    case C_AsyncFileOpenModeAppend:
    case C_AsyncFileOpenModeTruncate:
        return true;
    }
    return false;
}

galay::async::FileOpenMode to_cpp_open_mode(C_AsyncFileOpenMode mode)
{
    switch (mode) {
    case C_AsyncFileOpenModeRead:
        return galay::async::FileOpenMode::Read;
    case C_AsyncFileOpenModeWrite:
        return galay::async::FileOpenMode::Write;
    case C_AsyncFileOpenModeReadWrite:
        return galay::async::FileOpenMode::ReadWrite;
    case C_AsyncFileOpenModeAppend:
        return galay::async::FileOpenMode::Append;
    case C_AsyncFileOpenModeTruncate:
        return galay::async::FileOpenMode::Truncate;
    }
    return galay::async::FileOpenMode::Read;
}

galay::async::AsyncFile* to_cpp_file(galay_kernel_async_file_t* c_file)
{
    return static_cast<galay::async::AsyncFile*>(c_file->file);
}

const galay::async::AsyncFile* to_cpp_file(const galay_kernel_async_file_t* c_file)
{
    return static_cast<const galay::async::AsyncFile*>(c_file->file);
}

bool is_file_open(const galay::async::AsyncFile* file)
{
    return file != nullptr && !(file->handle() == GHandle::invalid());
}

C_AsyncFileResultCode from_cpp_io_error(const galay::kernel::IOError& error)
{
    if (galay::kernel::IOError::contains(error.code(), galay::kernel::kParamInvalid)) {
        return C_AsyncFileParameterInvalid;
    }
    if (galay::kernel::IOError::contains(error.code(), galay::kernel::kNotReady) ||
        galay::kernel::IOError::contains(error.code(), galay::kernel::kDisconnectError)) {
        return C_AsyncFileOperationInvalid;
    }
    return C_AsyncFileIOFailed;
}

#endif

bool timeout_fits_chrono(int64_t timeout_ms)
{
    if (timeout_ms <= 0) {
        return true;
    }
    using MillisecondsRep = std::chrono::milliseconds::rep;
    using NanosecondsRep = std::chrono::nanoseconds::rep;
    constexpr auto max_milliseconds_rep =
        static_cast<int64_t>(std::numeric_limits<MillisecondsRep>::max());
    constexpr auto max_milliseconds_for_nanoseconds =
        static_cast<int64_t>(std::numeric_limits<NanosecondsRep>::max() / 1000000);
    constexpr int64_t max_supported_milliseconds =
        max_milliseconds_rep < max_milliseconds_for_nanoseconds
            ? max_milliseconds_rep
            : max_milliseconds_for_nanoseconds;
    return timeout_ms <= max_supported_milliseconds;
}

GalayCoreCoroIOResultCode to_core_code(C_IOResultCode code)
{
    switch (code) {
    case C_IOResultOk:
        return GalayCoreCoroIOResultOk;
    case C_IOResultEof:
        return GalayCoreCoroIOResultEof;
    case C_IOResultTimeout:
        return GalayCoreCoroIOResultTimeout;
    case C_IOResultCancelled:
        return GalayCoreCoroIOResultCancelled;
    case C_IOResultInvalid:
        return GalayCoreCoroIOResultInvalid;
    case C_IOResultError:
        return GalayCoreCoroIOResultError;
    }
    return GalayCoreCoroIOResultError;
}

C_IOResultCode from_core_code(GalayCoreCoroIOResultCode code)
{
    switch (code) {
    case GalayCoreCoroIOResultOk:
        return C_IOResultOk;
    case GalayCoreCoroIOResultEof:
        return C_IOResultEof;
    case GalayCoreCoroIOResultTimeout:
        return C_IOResultTimeout;
    case GalayCoreCoroIOResultCancelled:
        return C_IOResultCancelled;
    case GalayCoreCoroIOResultInvalid:
        return C_IOResultInvalid;
    case GalayCoreCoroIOResultError:
        return C_IOResultError;
    }
    return C_IOResultError;
}

GalayCoreCoroIOResult to_core_result(C_IOResult result)
{
    return GalayCoreCoroIOResult{
        to_core_code(result.code),
        result.sys_errno,
        result.bytes,
        result.value,
        result.ptr,
    };
}

C_IOResult from_core_result(GalayCoreCoroIOResult result)
{
    return C_IOResult{
        from_core_code(result.code),
        result.sys_errno,
        result.bytes,
        result.value,
        result.ptr,
    };
}

void* current_io_scheduler()
{
    return static_cast<void*>(galay::kernel::coro_c::currentTaskOwnerScheduler());
}

struct WaitRequestScope {
    C_CoroWaitRequest request{nullptr};
    void* user_data = nullptr;

    ~WaitRequestScope()
    {
        if (request.request != nullptr) {
            C_IOResult cleanup_result = galay_coro_wait_request_destroy(&request);
            request.request = cleanup_result.code == C_IOResultOk ? nullptr : request.request;
        }
    }
};

C_IOResult merge_cleanup_result(C_IOResult primary, C_IOResult cleanup)
{
    return primary.code == C_IOResultOk && cleanup.code != C_IOResultOk
        ? cleanup
        : primary;
}

C_IOResult close_wait_scope(WaitRequestScope& scope)
{
    if (scope.request.request == nullptr) {
        return make_result(C_IOResultOk);
    }
    return galay_coro_wait_request_destroy(&scope.request);
}

GalayCoreCoroIOResult wait_request(void* ctx, int64_t timeout_ms)
{
    auto* scope = static_cast<WaitRequestScope*>(ctx);
    if (scope == nullptr) {
        return to_core_result(make_result(C_IOResultInvalid));
    }
    return to_core_result(galay_coro_wait(&scope->request, timeout_ms));
}

GalayCoreCoroIOResult complete_user_data(void* user_data,
                                         GalayCoreCoroIOResult result)
{
    return to_core_result(
        galay_coro_wait_event_user_data_complete(user_data, from_core_result(result)));
}

GalayCoreCoroIOResult release_user_data(void* user_data)
{
    return to_core_result(galay_coro_wait_event_user_data_release(user_data));
}

C_IOResult prepare_wait_user_data(WaitRequestScope& scope)
{
    C_IOResult created = galay_coro_wait_request_create(&scope.request);
    if (created.code != C_IOResultOk) {
        return created;
    }

    uint64_t generation = 0;
    C_IOResult prepared = galay_coro_wait_request_prepare(&scope.request, &generation);
    if (prepared.code != C_IOResultOk) {
        return prepared;
    }

    C_CoroWaitEventToken token{nullptr};
    C_IOResult acquired =
        galay_coro_wait_request_event_token_acquire(&scope.request, generation, &token);
    if (acquired.code != C_IOResultOk) {
        C_IOResult cancelled = galay_coro_wait_request_cancel(&scope.request, generation);
        return merge_cleanup_result(acquired, cancelled);
    }

    C_IOResult detached =
        galay_coro_wait_event_token_detach_user_data(&token, &scope.user_data);
    if (detached.code != C_IOResultOk) {
        C_IOResult released = galay_coro_wait_event_token_release(&token);
        detached = merge_cleanup_result(detached, released);
        C_IOResult cancelled = galay_coro_wait_request_cancel(&scope.request, generation);
        detached = merge_cleanup_result(detached, cancelled);
    }
    return detached;
}

GalayCoreCoroWaitOps make_wait_ops(WaitRequestScope& scope)
{
    return GalayCoreCoroWaitOps{
        wait_request,
        complete_user_data,
        release_user_data,
        &scope,
    };
}

template <typename Submit>
C_IOResult submit_with_wait(Submit&& submit)
{
    WaitRequestScope scope;
    C_IOResult prepared = prepare_wait_user_data(scope);
    if (prepared.code != C_IOResultOk) {
        return prepared;
    }

    GalayCoreCoroWaitOps wait_ops = make_wait_ops(scope);
    C_IOResult result = from_core_result(std::forward<Submit>(submit)(scope.user_data, &wait_ops));
    return merge_cleanup_result(result, close_wait_scope(scope));
}

} // namespace

extern "C" {

const char* galay_kernel_async_file_get_error(C_AsyncFileResultCode code)
{
    switch (code) {
    case C_AsyncFileSuccess:
        return "success";
    case C_AsyncFileParameterInvalid:
        return "parameter invalid";
    case C_AsyncFileMemoryAllocFailed:
        return "memory allocation failed";
    case C_AsyncFileIOFailed:
        return "io failed";
    case C_AsyncFileOperationInvalid:
        return "operation invalid";
    case C_AsyncFileOperationUnsupported:
        return "operation unsupported";
    }
    return "unknown async file error";
}

C_AsyncFileResultCode galay_kernel_async_file_create(galay_kernel_async_file_t* c_file)
{
    if (c_file == nullptr) {
        return C_AsyncFileParameterInvalid;
    }

    c_file->file = nullptr;
#if defined(USE_KQUEUE) || defined(USE_IOURING)
    auto* file = new (std::nothrow) galay::async::AsyncFile();
    if (file == nullptr) {
        return C_AsyncFileMemoryAllocFailed;
    }
    c_file->file = file;
    return C_AsyncFileSuccess;
#else
    return C_AsyncFileOperationUnsupported;
#endif
}

C_AsyncFileResultCode galay_kernel_async_file_destroy(galay_kernel_async_file_t* c_file)
{
    if (c_file == nullptr) {
        return C_AsyncFileParameterInvalid;
    }
#if defined(USE_KQUEUE) || defined(USE_IOURING)
    delete static_cast<galay::async::AsyncFile*>(c_file->file);
#endif
    c_file->file = nullptr;
    return C_AsyncFileSuccess;
}

C_AsyncFileResultCode galay_kernel_async_file_open(
    galay_kernel_async_file_t* c_file,
    const char* path,
    C_AsyncFileOpenMode mode [[maybe_unused]],
    int permissions [[maybe_unused]])
{
    if (c_file == nullptr || path == nullptr || path[0] == '\0') {
        return C_AsyncFileParameterInvalid;
    }
#if defined(USE_KQUEUE) || defined(USE_IOURING)
    if (c_file->file == nullptr || !is_valid_open_mode(mode) || permissions < 0) {
        return C_AsyncFileParameterInvalid;
    }
    auto* file = to_cpp_file(c_file);
    auto opened = file->open(std::string(path), to_cpp_open_mode(mode), permissions);
    return opened ? C_AsyncFileSuccess : from_cpp_io_error(opened.error());
#else
    return C_AsyncFileOperationUnsupported;
#endif
}

C_AsyncFileResultCode galay_kernel_async_file_size(
    const galay_kernel_async_file_t* c_file,
    size_t* size)
{
    if (c_file == nullptr || size == nullptr) {
        return C_AsyncFileParameterInvalid;
    }
#if defined(USE_KQUEUE) || defined(USE_IOURING)
    if (c_file->file == nullptr) {
        return C_AsyncFileParameterInvalid;
    }
    const auto* file = to_cpp_file(c_file);
    if (!is_file_open(file)) {
        return C_AsyncFileOperationInvalid;
    }
    auto current_size = file->size();
    if (!current_size) {
        return from_cpp_io_error(current_size.error());
    }
    *size = *current_size;
    return C_AsyncFileSuccess;
#else
    return C_AsyncFileOperationUnsupported;
#endif
}

C_AsyncFileResultCode galay_kernel_async_file_sync(galay_kernel_async_file_t* c_file)
{
    if (c_file == nullptr) {
        return C_AsyncFileParameterInvalid;
    }
#if defined(USE_KQUEUE) || defined(USE_IOURING)
    if (c_file->file == nullptr) {
        return C_AsyncFileParameterInvalid;
    }
    auto* file = to_cpp_file(c_file);
    if (!is_file_open(file)) {
        return C_AsyncFileOperationInvalid;
    }
    auto synced = file->sync();
    return synced ? C_AsyncFileSuccess : from_cpp_io_error(synced.error());
#else
    return C_AsyncFileOperationUnsupported;
#endif
}

C_IOResult galay_kernel_async_file_read(
    galay_kernel_async_file_t* file [[maybe_unused]],
    char* buffer [[maybe_unused]],
    size_t length [[maybe_unused]],
    int64_t offset [[maybe_unused]],
    int64_t timeout_ms [[maybe_unused]])
{
    if (file == nullptr || file->file == nullptr ||
        buffer == nullptr || length == 0 || !offset_fits_off_t(offset) ||
        !timeout_fits_chrono(timeout_ms)) {
        return make_result(C_IOResultInvalid);
    }
#if defined(USE_KQUEUE) || defined(USE_IOURING)
    void* scheduler = current_io_scheduler();
    if (scheduler == nullptr) {
        return make_result(C_IOResultInvalid);
    }
    if (timeout_ms == 0) {
        return make_result(C_IOResultTimeout);
    }
    auto* cpp_file = to_cpp_file(file);
    if (!is_file_open(cpp_file)) {
        return make_result(C_IOResultInvalid);
    }
    return submit_with_wait(
        [&](void* user_data, const GalayCoreCoroWaitOps* wait_ops) {
            return galay_core_coro_async_file_read(file->file,
                                                   scheduler,
                                                   buffer,
                                                   length,
                                                   offset,
                                                   timeout_ms,
                                                   user_data,
                                                   wait_ops);
        });
#else
    return make_result(C_IOResultError, ENOTSUP);
#endif
}

C_IOResult galay_kernel_async_file_write(
    galay_kernel_async_file_t* file [[maybe_unused]],
    const char* buffer [[maybe_unused]],
    size_t length [[maybe_unused]],
    int64_t offset [[maybe_unused]],
    int64_t timeout_ms [[maybe_unused]])
{
    if (file == nullptr || file->file == nullptr ||
        buffer == nullptr || length == 0 || !offset_fits_off_t(offset) ||
        !timeout_fits_chrono(timeout_ms)) {
        return make_result(C_IOResultInvalid);
    }
#if defined(USE_KQUEUE) || defined(USE_IOURING)
    void* scheduler = current_io_scheduler();
    if (scheduler == nullptr) {
        return make_result(C_IOResultInvalid);
    }
    if (timeout_ms == 0) {
        return make_result(C_IOResultTimeout);
    }
    auto* cpp_file = to_cpp_file(file);
    if (!is_file_open(cpp_file)) {
        return make_result(C_IOResultInvalid);
    }
    return submit_with_wait(
        [&](void* user_data, const GalayCoreCoroWaitOps* wait_ops) {
            return galay_core_coro_async_file_write(file->file,
                                                    scheduler,
                                                    buffer,
                                                    length,
                                                    offset,
                                                    timeout_ms,
                                                    user_data,
                                                    wait_ops);
        });
#else
    return make_result(C_IOResultError, ENOTSUP);
#endif
}

C_IOResult galay_kernel_async_file_close(
    galay_kernel_async_file_t* file [[maybe_unused]],
    int64_t timeout_ms [[maybe_unused]])
{
    if (file == nullptr || file->file == nullptr || !timeout_fits_chrono(timeout_ms)) {
        return make_result(C_IOResultInvalid);
    }
#if defined(USE_KQUEUE) || defined(USE_IOURING)
    void* scheduler = current_io_scheduler();
    if (scheduler == nullptr) {
        return make_result(C_IOResultInvalid);
    }
    return from_core_result(
        galay_core_coro_async_file_close(file->file, scheduler, timeout_ms));
#else
    return make_result(C_IOResultError, ENOTSUP);
#endif
}

} // extern "C"
