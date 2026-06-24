#include "async_file_c.h"

#include "../../../cpp/galay-kernel/async/async_file.h"
#include "../../../cpp/galay-kernel/core/runtime.h"

#include <limits>
#include <new>
#include <string>
#include <sys/types.h>

namespace
{

#if defined(USE_KQUEUE) || defined(USE_IOURING)

bool is_valid_open_mode(C_AsyncFileOpenMode mode)
{
    switch (mode)
    {
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
    switch (mode)
    {
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

galay::kernel::Runtime* to_cpp_runtime(galay_kernel_runtime_t* runtime)
{
    return static_cast<galay::kernel::Runtime*>(runtime->runtime);
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

bool offset_fits_off_t(size_t offset)
{
    return offset <= static_cast<size_t>(std::numeric_limits<off_t>::max());
}

C_AsyncFileResultCode from_cpp_io_error(const galay::kernel::IOError& error)
{
    if (galay::kernel::IOError::contains(error.code(), galay::kernel::kParamInvalid))
    {
        return C_AsyncFileParameterInvalid;
    }
    if (galay::kernel::IOError::contains(error.code(), galay::kernel::kNotReady) ||
        galay::kernel::IOError::contains(error.code(), galay::kernel::kDisconnectError))
    {
        return C_AsyncFileOperationInvalid;
    }
    return C_AsyncFileIOFailed;
}

galay::kernel::Task<void> c_api_read(
    galay::async::AsyncFile* file,
    char* buffer,
    size_t length,
    size_t offset,
    galay_kernel_async_file_read_callback_t callback,
    void* ctx)
{
    auto received = co_await file->read(buffer, length, static_cast<off_t>(offset));
    galay_kernel_async_file_read_result_t result{};
    result.code = received ? C_AsyncFileSuccess : from_cpp_io_error(received.error());
    result.buffer = buffer;
    result.length = length;
    result.offset = offset;
    result.bytes = received ? *received : 0;
    callback(&result, ctx);
    co_return;
}

galay::kernel::Task<void> c_api_write(
    galay::async::AsyncFile* file,
    const char* buffer,
    size_t length,
    size_t offset,
    galay_kernel_async_file_write_callback_t callback,
    void* ctx)
{
    auto sent = co_await file->write(buffer, length, static_cast<off_t>(offset));
    galay_kernel_async_file_write_result_t result{};
    result.code = sent ? C_AsyncFileSuccess : from_cpp_io_error(sent.error());
    result.buffer = buffer;
    result.length = length;
    result.offset = offset;
    result.bytes = sent ? *sent : 0;
    callback(&result, ctx);
    co_return;
}

galay::kernel::Task<void> c_api_close(
    galay::async::AsyncFile* file,
    galay_kernel_async_file_close_callback_t callback,
    void* ctx)
{
    auto closed = co_await file->close();
    callback(closed ? C_AsyncFileSuccess : from_cpp_io_error(closed.error()), ctx);
    co_return;
}

#endif

} // namespace

const char* galay_kernel_async_file_get_error(C_AsyncFileResultCode code)
{
    switch (code)
    {
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
    case C_AsyncFileRuntimeNotRunning:
        return "runtime not running";
    case C_AsyncFileRuntimeSpawnFailed:
        return "runtime spawn failed";
    }
    return "unknown async file error";
}

C_AsyncFileResultCode galay_kernel_async_file_create(galay_kernel_async_file_t* c_file)
{
    if (c_file == nullptr)
    {
        return C_AsyncFileParameterInvalid;
    }

    c_file->file = nullptr;
#if defined(USE_KQUEUE) || defined(USE_IOURING)
    auto* file = new (std::nothrow) galay::async::AsyncFile();
    if (file == nullptr)
    {
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
    if (c_file == nullptr)
    {
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
    C_AsyncFileOpenMode mode,
    int permissions)
{
    if (c_file == nullptr || path == nullptr || path[0] == '\0')
    {
        return C_AsyncFileParameterInvalid;
    }

#if defined(USE_KQUEUE) || defined(USE_IOURING)
    if (c_file->file == nullptr || !is_valid_open_mode(mode) || permissions < 0)
    {
        return C_AsyncFileParameterInvalid;
    }

    auto* file = to_cpp_file(c_file);
    auto opened = file->open(std::string(path), to_cpp_open_mode(mode), permissions);
    return opened ? C_AsyncFileSuccess : from_cpp_io_error(opened.error());
#else
    (void)mode;
    (void)permissions;
    return C_AsyncFileOperationUnsupported;
#endif
}

C_AsyncFileResultCode galay_kernel_async_file_size(
    const galay_kernel_async_file_t* c_file,
    size_t* size)
{
    if (c_file == nullptr || size == nullptr)
    {
        return C_AsyncFileParameterInvalid;
    }

#if defined(USE_KQUEUE) || defined(USE_IOURING)
    if (c_file->file == nullptr)
    {
        return C_AsyncFileParameterInvalid;
    }

    const auto* file = to_cpp_file(c_file);
    if (!is_file_open(file))
    {
        return C_AsyncFileOperationInvalid;
    }

    auto current_size = file->size();
    if (!current_size)
    {
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
    if (c_file == nullptr)
    {
        return C_AsyncFileParameterInvalid;
    }

#if defined(USE_KQUEUE) || defined(USE_IOURING)
    if (c_file->file == nullptr)
    {
        return C_AsyncFileParameterInvalid;
    }

    auto* file = to_cpp_file(c_file);
    if (!is_file_open(file))
    {
        return C_AsyncFileOperationInvalid;
    }

    auto synced = file->sync();
    return synced ? C_AsyncFileSuccess : from_cpp_io_error(synced.error());
#else
    return C_AsyncFileOperationUnsupported;
#endif
}

C_AsyncFileResultCode galay_kernel_async_file_read(
    galay_kernel_runtime_t* runtime,
    galay_kernel_async_file_t* c_file,
    char* buffer,
    size_t length,
    size_t offset,
    galay_kernel_async_file_read_callback_t callback,
    void* ctx)
{
    if (runtime == nullptr ||
        c_file == nullptr ||
        buffer == nullptr || length == 0 ||
        callback == nullptr)
    {
        return C_AsyncFileParameterInvalid;
    }

#if defined(USE_KQUEUE) || defined(USE_IOURING)
    if (runtime->runtime == nullptr || c_file->file == nullptr || !offset_fits_off_t(offset))
    {
        return C_AsyncFileParameterInvalid;
    }

    auto* cpp_runtime = to_cpp_runtime(runtime);
    if (!cpp_runtime->isRunning())
    {
        return C_AsyncFileRuntimeNotRunning;
    }

    auto* file = to_cpp_file(c_file);
    if (!is_file_open(file))
    {
        return C_AsyncFileOperationInvalid;
    }

    auto spawned = cpp_runtime->spawn(c_api_read(file, buffer, length, offset, callback, ctx));
    return spawned ? C_AsyncFileSuccess : C_AsyncFileRuntimeSpawnFailed;
#else
    (void)offset;
    (void)ctx;
    return C_AsyncFileOperationUnsupported;
#endif
}

C_AsyncFileResultCode galay_kernel_async_file_write(
    galay_kernel_runtime_t* runtime,
    galay_kernel_async_file_t* c_file,
    const char* buffer,
    size_t length,
    size_t offset,
    galay_kernel_async_file_write_callback_t callback,
    void* ctx)
{
    if (runtime == nullptr ||
        c_file == nullptr ||
        buffer == nullptr || length == 0 ||
        callback == nullptr)
    {
        return C_AsyncFileParameterInvalid;
    }

#if defined(USE_KQUEUE) || defined(USE_IOURING)
    if (runtime->runtime == nullptr || c_file->file == nullptr || !offset_fits_off_t(offset))
    {
        return C_AsyncFileParameterInvalid;
    }

    auto* cpp_runtime = to_cpp_runtime(runtime);
    if (!cpp_runtime->isRunning())
    {
        return C_AsyncFileRuntimeNotRunning;
    }

    auto* file = to_cpp_file(c_file);
    if (!is_file_open(file))
    {
        return C_AsyncFileOperationInvalid;
    }

    auto spawned = cpp_runtime->spawn(c_api_write(file, buffer, length, offset, callback, ctx));
    return spawned ? C_AsyncFileSuccess : C_AsyncFileRuntimeSpawnFailed;
#else
    (void)offset;
    (void)ctx;
    return C_AsyncFileOperationUnsupported;
#endif
}

C_AsyncFileResultCode galay_kernel_async_file_close(
    galay_kernel_runtime_t* runtime,
    galay_kernel_async_file_t* c_file,
    galay_kernel_async_file_close_callback_t callback,
    void* ctx)
{
    if (runtime == nullptr ||
        c_file == nullptr ||
        callback == nullptr)
    {
        return C_AsyncFileParameterInvalid;
    }

#if defined(USE_KQUEUE) || defined(USE_IOURING)
    if (runtime->runtime == nullptr || c_file->file == nullptr)
    {
        return C_AsyncFileParameterInvalid;
    }

    auto* cpp_runtime = to_cpp_runtime(runtime);
    if (!cpp_runtime->isRunning())
    {
        return C_AsyncFileRuntimeNotRunning;
    }

    auto* file = to_cpp_file(c_file);
    if (!is_file_open(file))
    {
        return C_AsyncFileOperationInvalid;
    }

    auto spawned = cpp_runtime->spawn(c_api_close(file, callback, ctx));
    return spawned ? C_AsyncFileSuccess : C_AsyncFileRuntimeSpawnFailed;
#else
    (void)ctx;
    return C_AsyncFileOperationUnsupported;
#endif
}
