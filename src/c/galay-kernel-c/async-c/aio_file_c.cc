#include "aio_file_c.h"

#ifdef USE_EPOLL
#include "../../../cpp/galay-kernel/async/aio_file.h"
#include "../coro-c/coro_task_internal.hpp"

#include <new>
#include <string>
#include <utility>
#endif

#include <cerrno>
#include <cstdint>

namespace
{

bool is_valid_open_mode(C_AioFileOpenMode mode)
{
    return mode == C_AioFileOpenModeRead ||
           mode == C_AioFileOpenModeWrite ||
           mode == C_AioFileOpenModeReadWrite;
}

bool is_valid_alignment(size_t alignment)
{
    return alignment >= sizeof(void*) && (alignment & (alignment - 1)) == 0;
}

#ifdef USE_EPOLL

galay::async::AioOpenMode to_cpp_open_mode(C_AioFileOpenMode mode)
{
    switch (mode)
    {
    case C_AioFileOpenModeRead:
        return galay::async::AioOpenMode::Read;
    case C_AioFileOpenModeWrite:
        return galay::async::AioOpenMode::Write;
    case C_AioFileOpenModeReadWrite:
        return galay::async::AioOpenMode::ReadWrite;
    }
    return galay::async::AioOpenMode::ReadWrite;
}

galay::async::AioFile* to_cpp_file(galay_kernel_aio_file_t* c_file)
{
    return static_cast<galay::async::AioFile*>(c_file->file);
}

C_AioFileResultCode from_cpp_io_error(const galay::kernel::IOError& error)
{
    if (galay::kernel::IOError::contains(error.code(), galay::kernel::kParamInvalid))
    {
        return C_AioFileParameterInvalid;
    }
    if (galay::kernel::IOError::contains(error.code(), galay::kernel::kNotReady))
    {
        return C_AioFileOperationInvalid;
    }
    if (galay::kernel::IOError::contains(error.code(), galay::kernel::kNotRunningOnIOScheduler))
    {
        return C_AioFileIOFailed;
    }
    return C_AioFileIOFailed;
}

#endif

C_IOResult make_result(C_IOResultCode code, int sys_errno = 0)
{
    return C_IOResult{code, sys_errno, 0, 0, nullptr};
}

} // namespace

const char* galay_kernel_aio_file_get_error(C_AioFileResultCode code)
{
    switch (code)
    {
    case C_AioFileSuccess:
        return "success";
    case C_AioFileParameterInvalid:
        return "parameter invalid";
    case C_AioFileMemoryAllocFailed:
        return "memory allocation failed";
    case C_AioFileIOFailed:
        return "io failed";
    case C_AioFileOperationInvalid:
        return "operation invalid";
    case C_AioFileOperationUnsupported:
        return "operation unsupported";
    }
    return "unknown aio file error";
}

C_AioFileResultCode galay_kernel_aio_file_create(galay_kernel_aio_file_t* c_file, int max_events)
{
    if (c_file == nullptr || max_events <= 0)
    {
        return C_AioFileParameterInvalid;
    }

    c_file->file = nullptr;
#ifndef USE_EPOLL
    return C_AioFileOperationUnsupported;
#else
    auto* file = new (std::nothrow) galay::async::AioFile(max_events);
    if (file == nullptr)
    {
        return C_AioFileMemoryAllocFailed;
    }

    c_file->file = file;
    return C_AioFileSuccess;
#endif
}

C_AioFileResultCode galay_kernel_aio_file_destroy(galay_kernel_aio_file_t* c_file)
{
    if (c_file == nullptr)
    {
        return C_AioFileParameterInvalid;
    }

#ifndef USE_EPOLL
    c_file->file = nullptr;
    return C_AioFileOperationUnsupported;
#else
    delete to_cpp_file(c_file);
    c_file->file = nullptr;
    return C_AioFileSuccess;
#endif
}

C_AioFileResultCode galay_kernel_aio_file_open(
    galay_kernel_aio_file_t* c_file,
    const char* path,
    C_AioFileOpenMode mode,
    int permissions)
{
    if (c_file == nullptr || path == nullptr || path[0] == '\0' || !is_valid_open_mode(mode))
    {
        return C_AioFileParameterInvalid;
    }

#ifndef USE_EPOLL
    (void)mode;
    (void)permissions;
    return C_AioFileOperationUnsupported;
#else
    if (c_file->file == nullptr)
    {
        return C_AioFileParameterInvalid;
    }

    auto* file = to_cpp_file(c_file);
    if (file->isValid())
    {
        return C_AioFileOperationInvalid;
    }

    auto opened = file->open(std::string(path), to_cpp_open_mode(mode), permissions);
    return opened ? C_AioFileSuccess : from_cpp_io_error(opened.error());
#endif
}

C_AioFileResultCode galay_kernel_aio_file_pre_read(
    galay_kernel_aio_file_t* c_file,
    char* buffer,
    size_t length,
    off_t offset)
{
    if (c_file == nullptr || buffer == nullptr || length == 0 || offset < 0)
    {
        return C_AioFileParameterInvalid;
    }

#ifndef USE_EPOLL
    return C_AioFileOperationUnsupported;
#else
    if (c_file->file == nullptr)
    {
        return C_AioFileParameterInvalid;
    }

    auto* file = to_cpp_file(c_file);
    if (!file->isValid())
    {
        return C_AioFileOperationInvalid;
    }

    file->preRead(buffer, length, offset);
    return C_AioFileSuccess;
#endif
}

C_AioFileResultCode galay_kernel_aio_file_pre_write(
    galay_kernel_aio_file_t* c_file,
    const char* buffer,
    size_t length,
    off_t offset)
{
    if (c_file == nullptr || buffer == nullptr || length == 0 || offset < 0)
    {
        return C_AioFileParameterInvalid;
    }

#ifndef USE_EPOLL
    return C_AioFileOperationUnsupported;
#else
    if (c_file->file == nullptr)
    {
        return C_AioFileParameterInvalid;
    }

    auto* file = to_cpp_file(c_file);
    if (!file->isValid())
    {
        return C_AioFileOperationInvalid;
    }

    file->preWrite(buffer, length, offset);
    return C_AioFileSuccess;
#endif
}

C_AioFileResultCode galay_kernel_aio_file_clear(galay_kernel_aio_file_t* c_file)
{
    if (c_file == nullptr)
    {
        return C_AioFileParameterInvalid;
    }

#ifndef USE_EPOLL
    return C_AioFileOperationUnsupported;
#else
    if (c_file->file == nullptr)
    {
        return C_AioFileParameterInvalid;
    }

    to_cpp_file(c_file)->clear();
    return C_AioFileSuccess;
#endif
}

C_AioFileResultCode galay_kernel_aio_file_close(galay_kernel_aio_file_t* c_file)
{
    if (c_file == nullptr)
    {
        return C_AioFileParameterInvalid;
    }

#ifndef USE_EPOLL
    return C_AioFileOperationUnsupported;
#else
    if (c_file->file == nullptr)
    {
        return C_AioFileParameterInvalid;
    }

    to_cpp_file(c_file)->close();
    return C_AioFileSuccess;
#endif
}

C_AioFileResultCode galay_kernel_aio_file_size(
    galay_kernel_aio_file_t* c_file,
    size_t* size)
{
    if (c_file == nullptr || size == nullptr)
    {
        return C_AioFileParameterInvalid;
    }

#ifndef USE_EPOLL
    *size = 0;
    return C_AioFileOperationUnsupported;
#else
    if (c_file->file == nullptr)
    {
        return C_AioFileParameterInvalid;
    }

    auto* file = to_cpp_file(c_file);
    if (!file->isValid())
    {
        return C_AioFileOperationInvalid;
    }

    auto file_size = file->size();
    if (!file_size)
    {
        return from_cpp_io_error(file_size.error());
    }

    *size = *file_size;
    return C_AioFileSuccess;
#endif
}

C_AioFileResultCode galay_kernel_aio_file_sync(galay_kernel_aio_file_t* c_file)
{
    if (c_file == nullptr)
    {
        return C_AioFileParameterInvalid;
    }

#ifndef USE_EPOLL
    return C_AioFileOperationUnsupported;
#else
    if (c_file->file == nullptr)
    {
        return C_AioFileParameterInvalid;
    }

    auto* file = to_cpp_file(c_file);
    if (!file->isValid())
    {
        return C_AioFileOperationInvalid;
    }

    auto synced = file->sync();
    return synced ? C_AioFileSuccess : from_cpp_io_error(synced.error());
#endif
}

C_AioFileResultCode galay_kernel_aio_file_alloc_aligned_buffer(
    size_t size,
    size_t alignment,
    char** buffer)
{
    if (buffer == nullptr || size == 0 || !is_valid_alignment(alignment))
    {
        return C_AioFileParameterInvalid;
    }

    *buffer = nullptr;
#ifndef USE_EPOLL
    return C_AioFileOperationUnsupported;
#else
    auto* allocated = galay::async::AioFile::allocAlignedBuffer(size, alignment);
    if (allocated == nullptr)
    {
        return C_AioFileMemoryAllocFailed;
    }

    *buffer = allocated;
    return C_AioFileSuccess;
#endif
}

C_AioFileResultCode galay_kernel_aio_file_free_aligned_buffer(char* buffer)
{
#ifndef USE_EPOLL
    (void)buffer;
    return C_AioFileOperationUnsupported;
#else
    galay::async::AioFile::freeAlignedBuffer(buffer);
    return C_AioFileSuccess;
#endif
}

C_IOResult galay_kernel_aio_file_commit(
    galay_kernel_aio_file_t* c_file,
    ssize_t* results,
    size_t result_capacity,
    size_t* out_count,
    int64_t timeout_ms)
{
    if (c_file == nullptr || results == nullptr || result_capacity == 0 || out_count == nullptr) {
        return make_result(C_IOResultInvalid);
    }

#ifndef USE_EPOLL
    (void)timeout_ms;
    return make_result(C_IOResultError, ENOTSUP);
#else
    if (c_file->file == nullptr)
    {
        return make_result(C_IOResultInvalid);
    }
    if (galay::kernel::coro_c::currentTaskOwnerScheduler() == nullptr) {
        return make_result(C_IOResultInvalid);
    }
    auto* file = to_cpp_file(c_file);
    if (!file->isValid()) {
        return make_result(C_IOResultInvalid);
    }
    if (timeout_ms == 0) {
        return make_result(C_IOResultTimeout);
    }

    auto committed = file->commit();
    if (committed.await_ready()) {
        auto resumed = committed.await_resume();
        if (!resumed) {
            return from_cpp_io_error(resumed.error()) == C_AioFileParameterInvalid
                ? make_result(C_IOResultInvalid)
                : make_result(C_IOResultError);
        }
        if (resumed->size() > result_capacity) {
            return make_result(C_IOResultInvalid);
        }
        *out_count = resumed->size();
        for (size_t i = 0; i < resumed->size(); ++i) {
            results[i] = (*resumed)[i];
        }
        return make_result(C_IOResultOk);
    }
    return make_result(C_IOResultError, ENOTSUP);
#endif
}
