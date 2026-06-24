#include "async_mutex_c.h"

#include "../../../cpp/galay-kernel/concurrency/async_mutex.h"
#include "../../../cpp/galay-kernel/core/runtime.h"

#include <chrono>
#include <new>

namespace
{

galay::kernel::Runtime* to_cpp_runtime(galay_kernel_runtime_t* c_runtime)
{
    return static_cast<galay::kernel::Runtime*>(c_runtime->runtime);
}

galay::kernel::AsyncMutex* to_cpp_mutex(galay_kernel_async_mutex_t* c_mutex)
{
    return static_cast<galay::kernel::AsyncMutex*>(c_mutex->mutex);
}

const galay::kernel::AsyncMutex* to_cpp_mutex(const galay_kernel_async_mutex_t* c_mutex)
{
    return static_cast<const galay::kernel::AsyncMutex*>(c_mutex->mutex);
}

C_AsyncMutexResultCode from_cpp_io_error(const galay::kernel::IOError& error)
{
    if (galay::kernel::IOError::contains(error.code(), galay::kernel::kTimeout))
    {
        return C_AsyncMutexTimeout;
    }
    if (galay::kernel::IOError::contains(error.code(), galay::kernel::kParamInvalid))
    {
        return C_AsyncMutexParameterInvalid;
    }
    if (galay::kernel::IOError::contains(error.code(), galay::kernel::kNotReady))
    {
        return C_AsyncMutexOperationInvalid;
    }
    return C_AsyncMutexIOFailed;
}

bool is_valid_timeout_ms(uint64_t timeout_ms)
{
    using Milliseconds = std::chrono::milliseconds;
    return timeout_ms <= static_cast<uint64_t>(Milliseconds::max().count());
}

galay::kernel::Task<void> c_api_lock(
    galay::kernel::AsyncMutex* mutex,
    galay_kernel_async_mutex_callback_t callback,
    void* ctx)
{
    auto locked = co_await mutex->lock();
    callback(locked ? C_AsyncMutexSuccess : from_cpp_io_error(locked.error()), ctx);
    co_return;
}

galay::kernel::Task<void> c_api_lock_timeout(
    galay::kernel::AsyncMutex* mutex,
    uint64_t timeout_ms,
    galay_kernel_async_mutex_callback_t callback,
    void* ctx)
{
    auto locked = co_await mutex->lock().timeout(
        std::chrono::milliseconds(static_cast<std::chrono::milliseconds::rep>(timeout_ms)));
    callback(locked ? C_AsyncMutexSuccess : from_cpp_io_error(locked.error()), ctx);
    co_return;
}

} // namespace

const char* galay_kernel_async_mutex_get_error(C_AsyncMutexResultCode code)
{
    switch (code)
    {
    case C_AsyncMutexSuccess:
        return "success";
    case C_AsyncMutexParameterInvalid:
        return "parameter invalid";
    case C_AsyncMutexMemoryAllocFailed:
        return "memory allocation failed";
    case C_AsyncMutexIOFailed:
        return "io failed";
    case C_AsyncMutexOperationInvalid:
        return "operation invalid";
    case C_AsyncMutexTimeout:
        return "timeout";
    case C_AsyncMutexRuntimeNotRunning:
        return "runtime not running";
    case C_AsyncMutexRuntimeSpawnFailed:
        return "runtime spawn failed";
    }
    return "unknown async mutex error";
}

C_AsyncMutexResultCode galay_kernel_async_mutex_create(galay_kernel_async_mutex_t* c_mutex)
{
    if (c_mutex == nullptr)
    {
        return C_AsyncMutexParameterInvalid;
    }

    c_mutex->mutex = nullptr;
    auto* mutex = new (std::nothrow) galay::kernel::AsyncMutex();
    if (mutex == nullptr)
    {
        return C_AsyncMutexMemoryAllocFailed;
    }

    c_mutex->mutex = mutex;
    return C_AsyncMutexSuccess;
}

C_AsyncMutexResultCode galay_kernel_async_mutex_destroy(galay_kernel_async_mutex_t* c_mutex)
{
    if (c_mutex == nullptr)
    {
        return C_AsyncMutexParameterInvalid;
    }

    delete to_cpp_mutex(c_mutex);
    c_mutex->mutex = nullptr;
    return C_AsyncMutexSuccess;
}

bool galay_kernel_async_mutex_is_locked(const galay_kernel_async_mutex_t* c_mutex)
{
    if (c_mutex == nullptr || c_mutex->mutex == nullptr)
    {
        return false;
    }

    return to_cpp_mutex(c_mutex)->isLocked();
}

C_AsyncMutexResultCode galay_kernel_async_mutex_unlock(galay_kernel_async_mutex_t* c_mutex)
{
    if (c_mutex == nullptr || c_mutex->mutex == nullptr)
    {
        return C_AsyncMutexParameterInvalid;
    }

    to_cpp_mutex(c_mutex)->unlock();
    return C_AsyncMutexSuccess;
}

C_AsyncMutexResultCode galay_kernel_async_mutex_lock(
    galay_kernel_runtime_t* runtime,
    galay_kernel_async_mutex_t* c_mutex,
    galay_kernel_async_mutex_callback_t callback,
    void* ctx)
{
    if (runtime == nullptr || runtime->runtime == nullptr ||
        c_mutex == nullptr || c_mutex->mutex == nullptr ||
        callback == nullptr)
    {
        return C_AsyncMutexParameterInvalid;
    }

    auto* cpp_runtime = to_cpp_runtime(runtime);
    if (!cpp_runtime->isRunning())
    {
        return C_AsyncMutexRuntimeNotRunning;
    }

    auto* mutex = to_cpp_mutex(c_mutex);
    auto spawned = cpp_runtime->spawn(c_api_lock(mutex, callback, ctx));
    return spawned ? C_AsyncMutexSuccess : C_AsyncMutexRuntimeSpawnFailed;
}

C_AsyncMutexResultCode galay_kernel_async_mutex_lock_timeout(
    galay_kernel_runtime_t* runtime,
    galay_kernel_async_mutex_t* c_mutex,
    uint64_t timeout_ms,
    galay_kernel_async_mutex_callback_t callback,
    void* ctx)
{
    if (runtime == nullptr || runtime->runtime == nullptr ||
        c_mutex == nullptr || c_mutex->mutex == nullptr ||
        callback == nullptr || !is_valid_timeout_ms(timeout_ms))
    {
        return C_AsyncMutexParameterInvalid;
    }

    auto* cpp_runtime = to_cpp_runtime(runtime);
    if (!cpp_runtime->isRunning())
    {
        return C_AsyncMutexRuntimeNotRunning;
    }

    auto* mutex = to_cpp_mutex(c_mutex);
    auto spawned = cpp_runtime->spawn(c_api_lock_timeout(mutex, timeout_ms, callback, ctx));
    return spawned ? C_AsyncMutexSuccess : C_AsyncMutexRuntimeSpawnFailed;
}
