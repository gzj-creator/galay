#include "c_coro_file_watcher_bridge.h"
#include "c_coro_operation_base.h"

#include <galay/cpp/galay-kernel/async/async_file_watcher.h>
#include <galay/cpp/galay-kernel/core/awaitable.h>
#include <galay/cpp/galay-kernel/core/io_scheduler.hpp>

#include <cstring>
#include <string>

namespace
{

using galay::async::AsyncFileWatcher;
using galay::kernel::FileWatchAwaitable;
using galay::kernel::IOController;
using galay::kernel::Scheduler;

using galay::bridge::detail::CoroOperationBase;
using galay::bridge::detail::C_IOResult;
using galay::bridge::detail::C_IOResultInvalid;
using galay::bridge::detail::C_IOResultOk;
using galay::bridge::detail::C_IOResultTimeout;
using galay::bridge::detail::from_io_error;
using galay::bridge::detail::make_result;
using galay::bridge::detail::perform_registered_io;
using galay::bridge::detail::timeout_fits_chrono;
using galay::bridge::detail::to_io_scheduler;
using galay::bridge::detail::valid_wait_ops;

AsyncFileWatcher* to_cpp_watcher(GalayCoreFileWatcher* watcher)
{
    return reinterpret_cast<AsyncFileWatcher*>(watcher);
}

void copy_name_to_result(const std::string& name, GalayCoreCoroFileWatchResult* result)
{
    const auto bytes = name.size() < sizeof(result->name) - 1
        ? name.size()
        : sizeof(result->name) - 1;
    if (bytes > 0) {
        std::memcpy(result->name, name.data(), bytes);
    }
    result->name[bytes] = '\0';
}

struct CoroFileWatcherOperation final:
    public FileWatchAwaitable,
    public CoroOperationBase<CoroFileWatcherOperation> {
    CoroFileWatcherOperation(IOController* controller,
                             Scheduler* scheduler,
                             void* user_data,
                             GalayCoreCoroWaitOps wait_ops,
                             GalayCoreCoroFileWatchResult* out_result)
#ifdef USE_KQUEUE
        : FileWatchAwaitable(controller,
                             m_buffer,
                             sizeof(m_buffer),
                             galay::kernel::FileWatchEvent::All)
#else
        : FileWatchAwaitable(controller, m_buffer, sizeof(m_buffer))
#endif
        , CoroOperationBase(scheduler, user_data, wait_ops)
        , m_out_result(out_result)
    {
        m_waker = makeWaker();
#ifdef USE_IOURING
        m_sqe_type = FILEWATCH;
#endif
    }

#ifdef USE_IOURING
    bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override
    {
        return guardedHandleComplete(
            [&]() { return FileWatchAwaitable::handleComplete(cqe, handle); });
    }
#else
    bool handleComplete(GHandle handle) override
    {
        return guardedHandleComplete(
            [&]() { return FileWatchAwaitable::handleComplete(handle); });
    }
#endif

    C_IOResult buildResultImpl() noexcept
    {
        if (!m_result) {
            return from_io_error(m_result.error());
        }
        GalayCoreCoroFileWatchResult result{};
        result.events = static_cast<GalayCoreCoroFileWatchEvent>(
            static_cast<unsigned int>(m_result->event));
        result.is_dir = m_result->isDir;
        copy_name_to_result(m_result->name, &result);
        *m_out_result = result;
        return make_result(C_IOResultOk);
    }

    GalayCoreCoroFileWatchResult* m_out_result = nullptr;
    char m_buffer[4096]{};
};

} // namespace

extern "C" {

GalayCoreCoroIOResult galay_core_coro_file_watcher_watch(
    GalayCoreFileWatcher* watcher_handle,
    GalayCoreIOScheduler* scheduler_handle,
    GalayCoreCoroFileWatchResult* out_result,
    int64_t timeout_ms,
    void* user_data,
    const GalayCoreCoroWaitOps* wait_ops)
{
    auto* watcher = to_cpp_watcher(watcher_handle);
    Scheduler* scheduler = to_io_scheduler(scheduler_handle);
    if (watcher == nullptr || out_result == nullptr ||
        scheduler == nullptr || !timeout_fits_chrono(timeout_ms) ||
        !valid_wait_ops(wait_ops)) {
        return make_result(C_IOResultInvalid);
    }
    if (timeout_ms == 0) {
        return make_result(C_IOResultTimeout);
    }
    if (user_data == nullptr || !watcher->isValid()) {
        return make_result(C_IOResultInvalid);
    }
    CoroFileWatcherOperation operation(watcher->getController(),
                                       scheduler,
                                       user_data,
                                       *wait_ops,
                                       out_result);
    return perform_registered_io<false, false>(watcher->getController(),
                                               scheduler,
                                               FILEWATCH,
                                               timeout_ms,
                                               operation,
                                               static_cast<FileWatchAwaitable*>(&operation));
}

} // extern "C"
