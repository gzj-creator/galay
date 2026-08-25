/**
 * @file static_file_reader.cc
 * @brief Native/fallback implementation of the HTTP static-file reader.
 */

#include "static_file_reader.h"

#include <galay/cpp/galay-kernel/async/async_waiter.h>
#include <galay/cpp/galay-kernel/core/runtime.h>

#if defined(USE_IOURING)
// io_uring submits regular-file reads to the kernel asynchronously. The
// kqueue file path is readiness based and performs pread in the scheduler,
// so it intentionally uses the blocking executor below.
#include <galay/cpp/galay-kernel/async/async_file.h>
#endif

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <functional>
#include <limits>
#include <memory>
#include <filesystem>
#include <sys/stat.h>
#include <unistd.h>
#include <type_traits>
#include <utility>

namespace galay::http
{

namespace
{

StaticFileReadError makeError(StaticFileReadErrorCode code,
                              size_t expected_bytes = 0,
                              size_t actual_bytes = 0,
                              int error_number = 0,
                              int close_error_number = 0) noexcept
{
    return StaticFileReadError{
        .expected_bytes = expected_bytes,
        .actual_bytes = actual_bytes,
        .error_number = error_number,
        .close_error_number = close_error_number,
        .code = code,
    };
}

std::expected<off_t, StaticFileReadError> checkedOffset(size_t offset,
                                                        size_t length) noexcept
{
    const auto max_offset = static_cast<uintmax_t>(std::numeric_limits<off_t>::max());
    if (static_cast<uintmax_t>(offset) > max_offset ||
        static_cast<uintmax_t>(length) > max_offset - static_cast<uintmax_t>(offset)) {
        return std::unexpected(makeError(StaticFileReadErrorCode::kInvalidRange,
                                          length,
                                          0,
                                          EOVERFLOW));
    }
    return static_cast<off_t>(offset);
}

#if !defined(USE_IOURING)

std::expected<std::string, StaticFileReadError> readDescriptorBlocking(int fd,
                                                                        size_t offset,
                                                                        size_t length)
{
    if (length == 0) {
        return std::string{};
    }
    if (fd < 0) {
        return std::unexpected(makeError(StaticFileReadErrorCode::kOpenFailed,
                                          length,
                                          0,
                                          EBADF));
    }

    auto checked_offset = checkedOffset(offset, length);
    if (!checked_offset.has_value()) {
        return std::unexpected(checked_offset.error());
    }

    std::string content(length, '\0');
    size_t total_read = 0;
    while (total_read < length) {
        const size_t remaining = length - total_read;
        const size_t read_size = std::min(
            remaining,
            static_cast<size_t>(std::numeric_limits<ssize_t>::max()));
        const ssize_t bytes_read = ::pread(fd,
                                           content.data() + total_read,
                                           read_size,
                                           *checked_offset + static_cast<off_t>(total_read));
        if (bytes_read < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::unexpected(makeError(StaticFileReadErrorCode::kReadFailed,
                                              length,
                                              total_read,
                                              errno));
        }
        if (bytes_read == 0) {
            return std::unexpected(makeError(StaticFileReadErrorCode::kShortRead,
                                              length,
                                              total_read));
        }
        total_read += static_cast<size_t>(bytes_read);
    }
    return content;
}

#endif

template<typename Result>
struct NotifyBlockingOperationOnExit
{
    using Waiter = galay::kernel::AsyncWaiter<Result>;

    explicit NotifyBlockingOperationOnExit(std::shared_ptr<Waiter> waiter) noexcept
        : waiter(std::move(waiter))
    {
    }

    ~NotifyBlockingOperationOnExit() noexcept
    {
        if (!notified) {
            (void)this->waiter->notify(
                std::unexpected(makeError(StaticFileReadErrorCode::kTaskFailed)));
        }
    }

    void notify(Result result)
    {
        (void)waiter->notify(std::move(result));
        notified = true;
    }

    std::shared_ptr<Waiter> waiter;
    bool notified = false;
};

template<typename F>
galay::kernel::Task<std::invoke_result_t<std::decay_t<F>&>> runBlockingOperation(F&& operation)
{
    using Operation = std::decay_t<F>;
    using Result = std::invoke_result_t<Operation&>;

    auto runtime = galay::kernel::RuntimeHandle::current();
    if (!runtime.has_value()) {
        co_return std::unexpected(makeError(StaticFileReadErrorCode::kRuntimeUnavailable));
    }

    using Waiter = galay::kernel::AsyncWaiter<Result>;
    auto waiter = std::make_shared<Waiter>();
    auto submitted = runtime->spawnBlocking(
        [operation = Operation(std::forward<F>(operation)), waiter]() mutable {
            NotifyBlockingOperationOnExit<Result> notification(waiter);
            auto result = std::invoke(operation);
            notification.notify(std::move(result));
        });
    if (!submitted.has_value() || !submitted->isValid()) {
        co_return std::unexpected(makeError(StaticFileReadErrorCode::kSubmitFailed));
    }

    auto result = co_await waiter->wait();
    if (!result.has_value()) {
        co_return std::unexpected(makeError(StaticFileReadErrorCode::kTaskFailed));
    }
    co_return std::move(result.value());
}

#if defined(USE_IOURING)

StaticFileReadError fromIoError(const galay::kernel::IOError& error,
                                StaticFileReadErrorCode fallback) noexcept
{
    const uint32_t io_code = static_cast<uint32_t>(error.code() & 0xffffffffu);
    const int system_code = static_cast<int>(error.code() >> 32);
    StaticFileReadErrorCode code = fallback;
    if (io_code == static_cast<uint32_t>(galay::kernel::kOpenFailed)) {
        code = StaticFileReadErrorCode::kOpenFailed;
    } else if (io_code == static_cast<uint32_t>(galay::kernel::kReadFailed)) {
        code = StaticFileReadErrorCode::kReadFailed;
    } else if (io_code == static_cast<uint32_t>(galay::kernel::kDisconnectError)) {
        code = StaticFileReadErrorCode::kCloseFailed;
    }
    return makeError(code, 0, 0, system_code);
}

#endif

} // namespace

const char* staticFileReadErrorName(StaticFileReadErrorCode code) noexcept
{
    switch (code) {
        case StaticFileReadErrorCode::kOpenFailed:
            return "open failed";
        case StaticFileReadErrorCode::kReadFailed:
            return "read failed";
        case StaticFileReadErrorCode::kShortRead:
            return "short read";
        case StaticFileReadErrorCode::kCloseFailed:
            return "close failed";
        case StaticFileReadErrorCode::kRuntimeUnavailable:
            return "runtime unavailable";
        case StaticFileReadErrorCode::kSubmitFailed:
            return "blocking submit failed";
        case StaticFileReadErrorCode::kTaskFailed:
            return "blocking task failed";
        case StaticFileReadErrorCode::kInvalidRange:
            return "invalid file range";
        case StaticFileReadErrorCode::kMetadataFailed:
            return "metadata failed";
    }
    return "unknown";
}

namespace
{

StaticFileMetadataResult inspectBlocking(const std::string& filePath)
{
    namespace fs = std::filesystem;
    std::error_code canonical_error;
    const fs::path canonical_path = fs::canonical(filePath, canonical_error);
    if (canonical_error) {
        return std::unexpected(makeError(StaticFileReadErrorCode::kMetadataFailed,
                                          0,
                                          0,
                                          canonical_error.value()));
    }

    std::error_code regular_error;
    if (!fs::is_regular_file(canonical_path, regular_error) || regular_error) {
        return std::unexpected(makeError(StaticFileReadErrorCode::kMetadataFailed,
                                          0,
                                          0,
                                          regular_error ? regular_error.value() : EINVAL));
    }

    std::error_code size_error;
    const uintmax_t raw_size = fs::file_size(canonical_path, size_error);
    if (size_error || raw_size > std::numeric_limits<size_t>::max()) {
        return std::unexpected(makeError(StaticFileReadErrorCode::kMetadataFailed,
                                          0,
                                          0,
                                          size_error ? size_error.value() : EOVERFLOW));
    }

    std::time_t last_modified = 0;
    struct stat metadata;
    if (::stat(canonical_path.c_str(), &metadata) == 0) {
        last_modified = metadata.st_mtime;
    } else {
        return std::unexpected(makeError(StaticFileReadErrorCode::kMetadataFailed,
                                          0,
                                          0,
                                          errno));
    }

    return StaticFileMetadata{
        .canonical_path = canonical_path.string(),
        .file_size = static_cast<size_t>(raw_size),
        .last_modified = last_modified,
    };
}

StaticFileDescriptorResult openForSendfileBlocking(const std::string& filePath)
{
    galay::kernel::FileDescriptor descriptor;
    auto opened = descriptor.open(filePath.c_str(), O_RDONLY | O_CLOEXEC);
    if (!opened.has_value()) {
        return std::unexpected(makeError(StaticFileReadErrorCode::kOpenFailed,
                                          0,
                                          0,
                                          static_cast<int>(opened.error().code() >> 32)));
    }
    return descriptor;
}

} // namespace

galay::kernel::Task<StaticFileMetadataResult> StaticFileReader::inspect(const std::string& filePath)
{
    auto result = co_await runBlockingOperation([filePath]() {
        return inspectBlocking(filePath);
    });
    if (!result.has_value()) {
        co_return std::unexpected(makeError(StaticFileReadErrorCode::kTaskFailed));
    }
    co_return std::move(result.value());
}

galay::kernel::Task<StaticFileDescriptorResult> StaticFileReader::openForSendfile(
    const std::string& filePath)
{
    auto result = co_await runBlockingOperation([filePath]() {
        return openForSendfileBlocking(filePath);
    });
    if (!result.has_value()) {
        co_return std::unexpected(makeError(StaticFileReadErrorCode::kTaskFailed));
    }
    co_return std::move(result.value());
}

galay::kernel::Task<StaticFileSessionResult> StaticFileReader::open(
    const std::string& filePath)
{
    auto result = co_await runBlockingOperation([filePath]() {
        return openForSendfileBlocking(filePath);
    });
    if (!result.has_value()) {
        co_return std::unexpected(makeError(StaticFileReadErrorCode::kTaskFailed));
    }
    StaticFileDescriptorResult descriptor_result = std::move(result.value());
    if (!descriptor_result.has_value()) {
        co_return std::unexpected(descriptor_result.error());
    }

    StaticFileSession session;
#if defined(USE_IOURING)
    session.m_file.adopt(descriptor_result.value().release());
#else
    session.m_file = std::move(descriptor_result.value());
#endif
    co_return std::move(session);
}

galay::kernel::Task<StaticFileReadResult> StaticFileReader::readAll(
    const std::string& filePath,
    size_t fileSize)
{
    return readAt(filePath, 0, fileSize);
}

galay::kernel::Task<StaticFileReadResult> StaticFileReader::readAt(
    const std::string& filePath,
    size_t offset,
    size_t length)
{
    auto opened = co_await open(filePath);
    if (!opened.has_value()) {
        co_return std::unexpected(makeError(StaticFileReadErrorCode::kTaskFailed,
                                            length));
    }
    StaticFileSessionResult session_result = std::move(opened.value());
    if (!session_result.has_value()) {
        co_return std::unexpected(session_result.error());
    }

    StaticFileSession session = std::move(session_result.value());
    auto result = co_await session.readAt(offset, length);
    if (!result.has_value()) {
        co_return std::unexpected(makeError(StaticFileReadErrorCode::kTaskFailed,
                                            length));
    }
    co_return std::move(result.value());
}

galay::kernel::Task<StaticFileReadResult> StaticFileSession::readAt(size_t offset,
                                                                     size_t length)
{
    if (length == 0) {
        co_return std::string{};
    }

    auto checked_offset = checkedOffset(offset, length);
    if (!checked_offset.has_value()) {
        co_return std::unexpected(checked_offset.error());
    }

#if defined(USE_IOURING)
    std::string content(length, '\0');
    size_t total_read = 0;
    while (total_read < length) {
        const size_t remaining = length - total_read;
        const size_t read_length = std::min(
            remaining,
            static_cast<size_t>(std::numeric_limits<unsigned>::max()));
        auto read_result = co_await m_file.read(content.data() + total_read,
                                               read_length,
                                               *checked_offset + static_cast<off_t>(total_read));
        if (!read_result.has_value()) {
            const auto read_error = fromIoError(read_result.error(),
                                                StaticFileReadErrorCode::kReadFailed);
            auto close_result = co_await m_file.close();
            if (!close_result.has_value()) {
                co_return std::unexpected(makeError(StaticFileReadErrorCode::kCloseFailed,
                                                    length,
                                                    total_read,
                                                    read_error.error_number,
                                                    static_cast<int>(close_result.error().code() >> 32)));
            }
            co_return std::unexpected(makeError(read_error.code,
                                                length,
                                                total_read,
                                                read_error.error_number));
        }
        if (read_result.value() == 0) {
            auto close_result = co_await m_file.close();
            const int close_error = close_result.has_value()
                ? 0
                : static_cast<int>(close_result.error().code() >> 32);
            co_return std::unexpected(makeError(StaticFileReadErrorCode::kShortRead,
                                                length,
                                                total_read,
                                                0,
                                                close_error));
        }
        total_read += read_result.value();
    }

    co_return content;
#else
    const int fd = m_file.get();
    auto result = co_await runBlockingOperation([fd, offset, length]() {
        return readDescriptorBlocking(fd, offset, length);
    });
    if (!result.has_value()) {
        co_return std::unexpected(makeError(StaticFileReadErrorCode::kTaskFailed,
                                            length));
    }
    co_return std::move(result.value());
#endif
}

} // namespace galay::http
