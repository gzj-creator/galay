#include "c_coro_async_file_bridge.h"
#include "c_coro_operation_base.h"

#include <galay/cpp/galay-kernel/async/async_file.h>
#include <galay/cpp/galay-kernel/core/awaitable.h>
#include <galay/cpp/galay-kernel/core/io_scheduler.hpp>

#include <cerrno>

namespace
{

using galay::bridge::detail::C_IOResult;
using galay::bridge::detail::C_IOResultError;
using galay::bridge::detail::C_IOResultInvalid;
using galay::bridge::detail::C_IOResultTimeout;
using galay::bridge::detail::make_result;
using galay::bridge::detail::timeout_fits_chrono;
using galay::bridge::detail::to_io_scheduler;
using galay::bridge::detail::valid_wait_ops;

#if defined(USE_KQUEUE) || defined(USE_IOURING)

using galay::async::AsyncFile;
using galay::kernel::FileReadAwaitable;
using galay::kernel::FileWriteAwaitable;
using galay::kernel::IOController;
using galay::kernel::Scheduler;

using galay::bridge::detail::CoroOperationBase;
using galay::bridge::detail::C_IOResultOk;
using galay::bridge::detail::from_io_error;
using galay::bridge::detail::perform_coro_close;
using galay::bridge::detail::perform_registered_io;

AsyncFile* to_cpp_file(GalayCoreAsyncFile* file)
{
    return reinterpret_cast<AsyncFile*>(file);
}

struct CoroFileReadOperation final:
    public FileReadAwaitable,
    public CoroOperationBase<CoroFileReadOperation> {
    CoroFileReadOperation(IOController* controller,
                          Scheduler* scheduler,
                          void* user_data,
                          GalayCoreCoroWaitOps wait_ops,
                          char* buffer,
                          size_t length,
                          off_t offset)
        : FileReadAwaitable(controller, buffer, length, offset)
        , CoroOperationBase(scheduler, user_data, wait_ops)
    {
        m_waker = makeWaker();
#ifdef USE_IOURING
        m_sqe_type = FILEREAD;
#endif
    }

#ifdef USE_IOURING
    bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override
    {
        return guardedHandleComplete(
            [&]() { return FileReadAwaitable::handleComplete(cqe, handle); });
    }
#else
    bool handleComplete(GHandle handle) override
    {
        return guardedHandleComplete(
            [&]() { return FileReadAwaitable::handleComplete(handle); });
    }
#endif

    C_IOResult buildResultImpl() noexcept
    {
        if (!m_result) {
            return from_io_error(m_result.error());
        }
        C_IOResult result = make_result(C_IOResultOk);
        result.bytes = *m_result;
        return result;
    }
};

struct CoroFileWriteOperation final:
    public FileWriteAwaitable,
    public CoroOperationBase<CoroFileWriteOperation> {
    CoroFileWriteOperation(IOController* controller,
                           Scheduler* scheduler,
                           void* user_data,
                           GalayCoreCoroWaitOps wait_ops,
                           const char* buffer,
                           size_t length,
                           off_t offset)
        : FileWriteAwaitable(controller, buffer, length, offset)
        , CoroOperationBase(scheduler, user_data, wait_ops)
    {
        m_waker = makeWaker();
#ifdef USE_IOURING
        m_sqe_type = FILEWRITE;
#endif
    }

#ifdef USE_IOURING
    bool handleComplete(struct io_uring_cqe* cqe, GHandle handle) override
    {
        return guardedHandleComplete(
            [&]() { return FileWriteAwaitable::handleComplete(cqe, handle); });
    }
#else
    bool handleComplete(GHandle handle) override
    {
        return guardedHandleComplete(
            [&]() { return FileWriteAwaitable::handleComplete(handle); });
    }
#endif

    C_IOResult buildResultImpl() noexcept
    {
        if (!m_result) {
            return from_io_error(m_result.error());
        }
        C_IOResult result = make_result(C_IOResultOk);
        result.bytes = *m_result;
        return result;
    }
};

#endif

} // namespace

extern "C" {

GalayCoreCoroIOResult galay_core_coro_async_file_read(GalayCoreAsyncFile* file_handle [[maybe_unused]],
                                                      GalayCoreIOScheduler* scheduler_handle [[maybe_unused]],
                                                      char* buffer [[maybe_unused]],
                                                      size_t length [[maybe_unused]],
                                                      int64_t offset [[maybe_unused]],
                                                      int64_t timeout_ms [[maybe_unused]],
                                                      void* user_data [[maybe_unused]],
                                                      const GalayCoreCoroWaitOps* wait_ops [[maybe_unused]])
{
#if defined(USE_KQUEUE) || defined(USE_IOURING)
    auto* file = to_cpp_file(file_handle);
    Scheduler* scheduler = to_io_scheduler(scheduler_handle);
    if (file == nullptr || buffer == nullptr || length == 0 || offset < 0 ||
        scheduler == nullptr || !timeout_fits_chrono(timeout_ms) ||
        !valid_wait_ops(wait_ops)) {
        return make_result(C_IOResultInvalid);
    }
    if (timeout_ms == 0) {
        return make_result(C_IOResultTimeout);
    }
    if (user_data == nullptr) {
        return make_result(C_IOResultInvalid);
    }
    CoroFileReadOperation operation(file->getController(),
                                    scheduler,
                                    user_data,
                                    *wait_ops,
                                    buffer,
                                    length,
                                    static_cast<off_t>(offset));
    return perform_registered_io<true, false>(file->getController(),
                                              scheduler,
                                              FILEREAD,
                                              timeout_ms,
                                              operation,
                                              static_cast<FileReadAwaitable*>(&operation));
#else
    return make_result(C_IOResultError, ENOTSUP);
#endif
}

GalayCoreCoroIOResult galay_core_coro_async_file_write(GalayCoreAsyncFile* file_handle [[maybe_unused]],
                                                       GalayCoreIOScheduler* scheduler_handle [[maybe_unused]],
                                                       const char* buffer [[maybe_unused]],
                                                       size_t length [[maybe_unused]],
                                                       int64_t offset [[maybe_unused]],
                                                       int64_t timeout_ms [[maybe_unused]],
                                                       void* user_data [[maybe_unused]],
                                                       const GalayCoreCoroWaitOps* wait_ops [[maybe_unused]])
{
#if defined(USE_KQUEUE) || defined(USE_IOURING)
    auto* file = to_cpp_file(file_handle);
    Scheduler* scheduler = to_io_scheduler(scheduler_handle);
    if (file == nullptr || buffer == nullptr || length == 0 || offset < 0 ||
        scheduler == nullptr || !timeout_fits_chrono(timeout_ms) ||
        !valid_wait_ops(wait_ops)) {
        return make_result(C_IOResultInvalid);
    }
    if (timeout_ms == 0) {
        return make_result(C_IOResultTimeout);
    }
    if (user_data == nullptr) {
        return make_result(C_IOResultInvalid);
    }
    CoroFileWriteOperation operation(file->getController(),
                                     scheduler,
                                     user_data,
                                     *wait_ops,
                                     buffer,
                                     length,
                                     static_cast<off_t>(offset));
    return perform_registered_io<true, false>(file->getController(),
                                              scheduler,
                                              FILEWRITE,
                                              timeout_ms,
                                              operation,
                                              static_cast<FileWriteAwaitable*>(&operation));
#else
    return make_result(C_IOResultError, ENOTSUP);
#endif
}

GalayCoreCoroIOResult galay_core_coro_async_file_close(GalayCoreAsyncFile* file_handle,
                                                       GalayCoreIOScheduler* scheduler_handle,
                                                       int64_t)
{
#if defined(USE_KQUEUE) || defined(USE_IOURING)
    auto* file = to_cpp_file(file_handle);
    Scheduler* scheduler = to_io_scheduler(scheduler_handle);
    if (file == nullptr || scheduler == nullptr) {
        return make_result(C_IOResultInvalid);
    }
    return perform_coro_close(file->getController(), scheduler);
#else
    return make_result(C_IOResultError, ENOTSUP);
#endif
}

} // extern "C"
