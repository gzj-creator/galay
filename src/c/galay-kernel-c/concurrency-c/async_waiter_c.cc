#include "async_waiter_c.h"

#include "../../../cpp/galay-kernel/common/error.h"
#include "../../../cpp/galay-kernel/concurrency/async_waiter.h"
#include "../../../cpp/galay-kernel/core/runtime.h"

#include <chrono>
#include <limits>
#include <new>

namespace
{

galay::kernel::Runtime* to_cpp_runtime(galay_kernel_runtime_t* runtime)
{
    return static_cast<galay::kernel::Runtime*>(runtime->runtime);
}

galay::kernel::AsyncWaiter<void>* to_cpp_waiter(galay_kernel_async_waiter_t* waiter)
{
    return static_cast<galay::kernel::AsyncWaiter<void>*>(waiter->waiter);
}

const galay::kernel::AsyncWaiter<void>* to_cpp_waiter(const galay_kernel_async_waiter_t* waiter)
{
    return static_cast<const galay::kernel::AsyncWaiter<void>*>(waiter->waiter);
}

C_AsyncWaiterResultCode from_cpp_io_error(const galay::kernel::IOError& error)
{
    if (galay::kernel::IOError::contains(error.code(), galay::kernel::kTimeout))
    {
        return C_AsyncWaiterTimeout;
    }
    if (galay::kernel::IOError::contains(error.code(), galay::kernel::kParamInvalid))
    {
        return C_AsyncWaiterParameterInvalid;
    }
    return C_AsyncWaiterIOFailed;
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

galay::kernel::Task<void> c_api_wait(
    galay::kernel::AsyncWaiter<void>* waiter,
    galay_kernel_async_waiter_callback_t callback,
    void* ctx)
{
    auto waited = co_await waiter->wait();
    callback(waited ? C_AsyncWaiterSuccess : from_cpp_io_error(waited.error()), ctx);
    co_return;
}

galay::kernel::Task<void> c_api_wait_timeout(
    galay::kernel::AsyncWaiter<void>* waiter,
    std::chrono::milliseconds timeout,
    galay_kernel_async_waiter_callback_t callback,
    void* ctx)
{
    auto waited = co_await waiter->wait().timeout(timeout);
    callback(waited ? C_AsyncWaiterSuccess : from_cpp_io_error(waited.error()), ctx);
    co_return;
}

} // namespace

const char* galay_kernel_async_waiter_get_error(C_AsyncWaiterResultCode code)
{
    switch (code)
    {
    case C_AsyncWaiterSuccess:
        return "success";
    case C_AsyncWaiterParameterInvalid:
        return "parameter invalid";
    case C_AsyncWaiterMemoryAllocFailed:
        return "memory allocation failed";
    case C_AsyncWaiterIOFailed:
        return "io failed";
    case C_AsyncWaiterOperationInvalid:
        return "operation invalid";
    case C_AsyncWaiterTimeout:
        return "timeout";
    case C_AsyncWaiterRuntimeNotRunning:
        return "runtime not running";
    case C_AsyncWaiterRuntimeSpawnFailed:
        return "runtime spawn failed";
    }
    return "unknown async waiter error";
}

C_AsyncWaiterResultCode galay_kernel_async_waiter_create(galay_kernel_async_waiter_t* c_waiter)
{
    if (c_waiter == nullptr)
    {
        return C_AsyncWaiterParameterInvalid;
    }

    c_waiter->waiter = nullptr;
    auto* waiter = new (std::nothrow) galay::kernel::AsyncWaiter<void>();
    if (waiter == nullptr)
    {
        return C_AsyncWaiterMemoryAllocFailed;
    }

    c_waiter->waiter = waiter;
    return C_AsyncWaiterSuccess;
}

C_AsyncWaiterResultCode galay_kernel_async_waiter_destroy(galay_kernel_async_waiter_t* c_waiter)
{
    if (c_waiter == nullptr)
    {
        return C_AsyncWaiterParameterInvalid;
    }

    delete to_cpp_waiter(c_waiter);
    c_waiter->waiter = nullptr;
    return C_AsyncWaiterSuccess;
}

bool galay_kernel_async_waiter_is_waiting(const galay_kernel_async_waiter_t* c_waiter)
{
    if (c_waiter == nullptr || c_waiter->waiter == nullptr)
    {
        return false;
    }
    return to_cpp_waiter(c_waiter)->isWaiting();
}

bool galay_kernel_async_waiter_is_ready(const galay_kernel_async_waiter_t* c_waiter)
{
    if (c_waiter == nullptr || c_waiter->waiter == nullptr)
    {
        return false;
    }
    return to_cpp_waiter(c_waiter)->isReady();
}

C_AsyncWaiterResultCode galay_kernel_async_waiter_notify(galay_kernel_async_waiter_t* c_waiter)
{
    if (c_waiter == nullptr || c_waiter->waiter == nullptr)
    {
        return C_AsyncWaiterParameterInvalid;
    }

    return to_cpp_waiter(c_waiter)->notify()
        ? C_AsyncWaiterSuccess
        : C_AsyncWaiterOperationInvalid;
}

C_AsyncWaiterResultCode galay_kernel_async_waiter_wait(
    galay_kernel_runtime_t* runtime,
    galay_kernel_async_waiter_t* c_waiter,
    galay_kernel_async_waiter_callback_t callback,
    void* ctx)
{
    if (runtime == nullptr || runtime->runtime == nullptr ||
        c_waiter == nullptr || c_waiter->waiter == nullptr ||
        callback == nullptr)
    {
        return C_AsyncWaiterParameterInvalid;
    }

    auto* cpp_runtime = to_cpp_runtime(runtime);
    if (!cpp_runtime->isRunning())
    {
        return C_AsyncWaiterRuntimeNotRunning;
    }

    auto spawned = cpp_runtime->spawn(c_api_wait(to_cpp_waiter(c_waiter), callback, ctx));
    return spawned ? C_AsyncWaiterSuccess : C_AsyncWaiterRuntimeSpawnFailed;
}

C_AsyncWaiterResultCode galay_kernel_async_waiter_wait_timeout(
    galay_kernel_runtime_t* runtime,
    galay_kernel_async_waiter_t* c_waiter,
    uint64_t timeout_ms,
    galay_kernel_async_waiter_callback_t callback,
    void* ctx)
{
    if (runtime == nullptr || runtime->runtime == nullptr ||
        c_waiter == nullptr || c_waiter->waiter == nullptr ||
        callback == nullptr || !timeout_fits_chrono(timeout_ms))
    {
        return C_AsyncWaiterParameterInvalid;
    }

    auto* cpp_runtime = to_cpp_runtime(runtime);
    if (!cpp_runtime->isRunning())
    {
        return C_AsyncWaiterRuntimeNotRunning;
    }

    const auto timeout = std::chrono::milliseconds(
        static_cast<std::chrono::milliseconds::rep>(timeout_ms));
    auto spawned = cpp_runtime->spawn(c_api_wait_timeout(to_cpp_waiter(c_waiter), timeout, callback, ctx));
    return spawned ? C_AsyncWaiterSuccess : C_AsyncWaiterRuntimeSpawnFailed;
}
