#include "file_watcher_c.h"

#include "../../../cpp/galay-kernel/async/file_watcher.h"
#include "../../../cpp/galay-kernel/core/runtime.h"

#include <cerrno>
#include <cstring>
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

#if defined(USE_IOURING) || defined(USE_EPOLL) || defined(USE_KQUEUE)

galay::kernel::Runtime* to_cpp_runtime(galay_kernel_runtime_t* runtime)
{
    return static_cast<galay::kernel::Runtime*>(runtime->runtime);
}

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

C_FileWatchEvent to_c_events(galay::kernel::FileWatchEvent events)
{
    return static_cast<C_FileWatchEvent>(static_cast<unsigned int>(events));
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

void copy_name_to_result(const std::string& name, galay_kernel_file_watcher_watch_result_t* result)
{
    const auto bytes = name.size() < sizeof(result->name) - 1
        ? name.size()
        : sizeof(result->name) - 1;
    if (bytes > 0)
    {
        std::memcpy(result->name, name.data(), bytes);
    }
    result->name[bytes] = '\0';
}

galay::kernel::Task<void> c_api_watch(
    galay::async::FileWatcher* watcher,
    galay_kernel_file_watcher_callback_t callback,
    void* ctx)
{
    auto watched = co_await watcher->watch();

    galay_kernel_file_watcher_watch_result_t result{};
    if (!watched)
    {
        result.code = from_cpp_io_error(watched.error());
        callback(&result, ctx);
        co_return;
    }

    result.code = C_FileWatcherSuccess;
    result.events = to_c_events(watched->event);
    result.is_dir = watched->isDir;
    copy_name_to_result(watched->name, &result);
    callback(&result, ctx);
    co_return;
}

#endif

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
    case C_FileWatcherRuntimeNotRunning:
        return "runtime not running";
    case C_FileWatcherRuntimeSpawnFailed:
        return "runtime spawn failed";
    case C_FileWatcherOperationUnsupported:
        return "operation unsupported";
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

C_FileWatcherResultCode galay_kernel_file_watcher_watch(
    galay_kernel_runtime_t* runtime,
    galay_kernel_file_watcher_t* c_watcher,
    galay_kernel_file_watcher_callback_t callback,
    void* ctx)
{
    if (runtime == nullptr || runtime->runtime == nullptr ||
        c_watcher == nullptr || callback == nullptr)
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

    auto* cpp_runtime = to_cpp_runtime(runtime);
    if (!cpp_runtime->isRunning())
    {
        return C_FileWatcherRuntimeNotRunning;
    }

    auto spawned = cpp_runtime->spawn(c_api_watch(to_cpp_watcher(c_watcher), callback, ctx));
    return spawned ? C_FileWatcherSuccess : C_FileWatcherRuntimeSpawnFailed;
#else
    return C_FileWatcherOperationUnsupported;
#endif
}
