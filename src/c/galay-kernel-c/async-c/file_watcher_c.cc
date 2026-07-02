#include "file_watcher_c.h"

#include "../../../cpp/galay-kernel/async/file_watcher.h"
#include "../coro-c/coro_task_internal.hpp"
#include "../coro-c/coro_wait_c.h"
#include <galay/c/galay-bridge-c/coro-c/c_coro_file_watcher_bridge.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <cstdint>
#include <limits>
#include <new>
#include <string>

namespace
{

constexpr bool kFileWatcherSupported =
#if defined(USE_IOURING) || defined(USE_EPOLL) || defined(USE_KQUEUE)
    true;
#else
    false;
#endif

bool is_valid_watch_events(C_FileWatchEvent events)
{
    const auto bits = static_cast<unsigned int>(events);
    return bits != 0u && (bits & ~static_cast<unsigned int>(C_FileWatchEventAll)) == 0u;
}

bool timeout_fits_chrono(uint64_t timeout_ms)
{
    using Rep = std::chrono::milliseconds::rep;
    if constexpr (std::numeric_limits<Rep>::is_signed)
    {
        return timeout_ms <= static_cast<uint64_t>(std::numeric_limits<Rep>::max());
    }
    return timeout_ms <= std::numeric_limits<Rep>::max();
}

#if defined(USE_IOURING) || defined(USE_EPOLL) || defined(USE_KQUEUE)

galay::async::FileWatcher* to_cpp_watcher(galay_kernel_file_watcher_t* watcher)
{
    return static_cast<galay::async::FileWatcher*>(watcher->watcher);
}

const galay::async::FileWatcher* to_cpp_watcher(const galay_kernel_file_watcher_t* watcher)
{
    return static_cast<const galay::async::FileWatcher*>(watcher->watcher);
}

galay::kernel::FileWatchEvent to_cpp_events(C_FileWatchEvent events)
{
    return static_cast<galay::kernel::FileWatchEvent>(static_cast<unsigned int>(events));
}

C_FileWatcherResultCode from_cpp_io_error(const galay::kernel::IOError& error)
{
    const auto code = error.code();
    const auto system_code = static_cast<unsigned int>(code >> 32u);
    if (galay::kernel::IOError::contains(code, galay::kernel::kParamInvalid) ||
        system_code == static_cast<unsigned int>(EINVAL))
    {
        return C_FileWatcherParameterInvalid;
    }
    return C_FileWatcherIOFailed;
}

#endif

C_IOResult make_result(C_IOResultCode code, int sys_errno = 0)
{
    return C_IOResult{code, sys_errno, 0, 0, nullptr};
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

GalayCoreIOScheduler* current_io_scheduler()
{
    return reinterpret_cast<GalayCoreIOScheduler*>(
        galay::kernel::coro_c::currentTaskOwnerScheduler());
}

GalayCoreFileWatcher* to_core_watcher(void* watcher)
{
    return reinterpret_cast<GalayCoreFileWatcher*>(watcher);
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

const char* galay_kernel_file_watcher_get_error(C_FileWatcherResultCode code)
{
    switch (code)
    {
    case C_FileWatcherSuccess:
        return "success";
    case C_FileWatcherParameterInvalid:
        return "parameter invalid";
    case C_FileWatcherMemoryAllocFailed:
        return "memory allocation failed";
    case C_FileWatcherIOFailed:
        return "io failed";
    case C_FileWatcherOperationInvalid:
        return "operation invalid";
    case C_FileWatcherOperationUnsupported:
        return "operation unsupported";
    case C_FileWatcherTimeout:
        return "timeout";
    }
    return "unknown file watcher error";
}

C_FileWatcherResultCode galay_kernel_file_watcher_create(
    galay_kernel_file_watcher_t* c_watcher)
{
    if (c_watcher == nullptr)
    {
        return C_FileWatcherParameterInvalid;
    }

    c_watcher->watcher = nullptr;
    if (!kFileWatcherSupported)
    {
        return C_FileWatcherOperationUnsupported;
    }

#if defined(USE_IOURING) || defined(USE_EPOLL) || defined(USE_KQUEUE)
    auto* watcher = new (std::nothrow) galay::async::FileWatcher();
    if (watcher == nullptr)
    {
        return C_FileWatcherMemoryAllocFailed;
    }

    c_watcher->watcher = watcher;
    return C_FileWatcherSuccess;
#else
    return C_FileWatcherOperationUnsupported;
#endif
}

C_FileWatcherResultCode galay_kernel_file_watcher_destroy(
    galay_kernel_file_watcher_t* c_watcher)
{
    if (c_watcher == nullptr)
    {
        return C_FileWatcherParameterInvalid;
    }

#if defined(USE_IOURING) || defined(USE_EPOLL) || defined(USE_KQUEUE)
    delete to_cpp_watcher(c_watcher);
#endif
    c_watcher->watcher = nullptr;
    return C_FileWatcherSuccess;
}

C_FileWatcherResultCode galay_kernel_file_watcher_add_watch(
    galay_kernel_file_watcher_t* c_watcher,
    const char* path,
    C_FileWatchEvent events,
    int* watch_descriptor)
{
    if (c_watcher == nullptr || path == nullptr || path[0] == '\0' ||
        watch_descriptor == nullptr || !is_valid_watch_events(events))
    {
        return C_FileWatcherParameterInvalid;
    }

    if (!kFileWatcherSupported)
    {
        return C_FileWatcherOperationUnsupported;
    }

#if defined(USE_IOURING) || defined(USE_EPOLL) || defined(USE_KQUEUE)
    if (c_watcher->watcher == nullptr)
    {
        return C_FileWatcherParameterInvalid;
    }

    auto added = to_cpp_watcher(c_watcher)->addWatch(path, to_cpp_events(events));
    if (!added)
    {
        return from_cpp_io_error(added.error());
    }

    *watch_descriptor = *added;
    return C_FileWatcherSuccess;
#else
    return C_FileWatcherOperationUnsupported;
#endif
}

C_FileWatcherResultCode galay_kernel_file_watcher_remove_watch(
    galay_kernel_file_watcher_t* c_watcher,
    int watch_descriptor)
{
    if (c_watcher == nullptr || watch_descriptor < 0)
    {
        return C_FileWatcherParameterInvalid;
    }

    if (!kFileWatcherSupported)
    {
        return C_FileWatcherOperationUnsupported;
    }

#if defined(USE_IOURING) || defined(USE_EPOLL) || defined(USE_KQUEUE)
    if (c_watcher->watcher == nullptr)
    {
        return C_FileWatcherParameterInvalid;
    }

    auto removed = to_cpp_watcher(c_watcher)->removeWatch(watch_descriptor);
    return removed ? C_FileWatcherSuccess : from_cpp_io_error(removed.error());
#else
    return C_FileWatcherOperationUnsupported;
#endif
}

C_FileWatcherResultCode galay_kernel_file_watcher_get_path(
    const galay_kernel_file_watcher_t* c_watcher,
    int watch_descriptor,
    char* buffer,
    size_t buffer_size)
{
    if (c_watcher == nullptr || watch_descriptor < 0 || buffer == nullptr || buffer_size == 0)
    {
        return C_FileWatcherParameterInvalid;
    }

    if (!kFileWatcherSupported)
    {
        return C_FileWatcherOperationUnsupported;
    }

#if defined(USE_IOURING) || defined(USE_EPOLL) || defined(USE_KQUEUE)
    if (c_watcher->watcher == nullptr)
    {
        return C_FileWatcherParameterInvalid;
    }

    const auto path = to_cpp_watcher(c_watcher)->getPath(watch_descriptor);
    if (path.empty() || path.size() + 1 > buffer_size)
    {
        return C_FileWatcherParameterInvalid;
    }

    std::memcpy(buffer, path.c_str(), path.size() + 1);
    return C_FileWatcherSuccess;
#else
    return C_FileWatcherOperationUnsupported;
#endif
}

C_IOResult galay_kernel_file_watcher_watch(
    galay_kernel_file_watcher_t* c_watcher [[maybe_unused]],
    galay_kernel_file_watcher_watch_result_t* out_result [[maybe_unused]],
    int64_t timeout_ms [[maybe_unused]])
{
#if defined(USE_IOURING) || defined(USE_EPOLL) || defined(USE_KQUEUE)
    GalayCoreIOScheduler* scheduler = current_io_scheduler();
    if (c_watcher == nullptr || c_watcher->watcher == nullptr ||
        out_result == nullptr || scheduler == nullptr || !timeout_fits_chrono(timeout_ms)) {
        return make_result(C_IOResultInvalid);
    }
    if (timeout_ms == 0) {
        out_result->code = C_FileWatcherTimeout;
        out_result->events = C_FileWatchEventNone;
        out_result->name[0] = '\0';
        out_result->is_dir = false;
        return make_result(C_IOResultTimeout);
    }
    GalayCoreCoroFileWatchResult core_result{};
    C_IOResult result = submit_with_wait(
        [&](void* user_data, const GalayCoreCoroWaitOps* wait_ops) {
            return galay_core_coro_file_watcher_watch(to_core_watcher(c_watcher->watcher),
                                                      scheduler,
                                                      &core_result,
                                                      timeout_ms,
                                                      user_data,
                                                      wait_ops);
        });
    if (result.code == C_IOResultOk) {
        out_result->code = C_FileWatcherSuccess;
        out_result->events = static_cast<C_FileWatchEvent>(
            static_cast<unsigned int>(core_result.events));
        std::memcpy(out_result->name, core_result.name, sizeof(out_result->name));
        out_result->name[sizeof(out_result->name) - 1] = '\0';
        out_result->is_dir = core_result.is_dir;
    } else if (result.code == C_IOResultTimeout) {
        out_result->code = C_FileWatcherTimeout;
        out_result->events = C_FileWatchEventNone;
        out_result->name[0] = '\0';
        out_result->is_dir = false;
    } else if (result.code == C_IOResultInvalid) {
        out_result->code = C_FileWatcherParameterInvalid;
        out_result->events = C_FileWatchEventNone;
        out_result->name[0] = '\0';
        out_result->is_dir = false;
    } else {
        out_result->code = C_FileWatcherIOFailed;
        out_result->events = C_FileWatchEventNone;
        out_result->name[0] = '\0';
        out_result->is_dir = false;
    }
    return result;
#else
    return make_result(C_IOResultError, ENOTSUP);
#endif
}
