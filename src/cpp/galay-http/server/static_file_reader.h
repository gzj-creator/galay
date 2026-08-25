/**
 * @file static_file_reader.h
 * @brief Unified asynchronous static-file reads for the HTTP server.
 *
 * The reader selects the native AsyncFile backend when the IO backend provides
 * asynchronous regular-file operations. Backends without that guarantee move
 * the complete blocking operation to Runtime's blocking executor instead.
 */

#ifndef GALAY_HTTP_STATIC_FILE_READER_H
#define GALAY_HTTP_STATIC_FILE_READER_H

#include <galay/cpp/galay-kernel/core/task.h>
#include <galay/cpp/galay-kernel/common/file_descriptor.h>

#if defined(USE_IOURING)
#include <galay/cpp/galay-kernel/async/async_file.h>
#endif

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <expected>
#include <string>

namespace galay::http
{

enum class StaticFileReadErrorCode : uint8_t
{
    kOpenFailed,
    kReadFailed,
    kShortRead,
    kCloseFailed,
    kRuntimeUnavailable,
    kSubmitFailed,
    kTaskFailed,
    kInvalidRange,
    kMetadataFailed,
};

struct StaticFileReadError
{
    size_t expected_bytes = 0;
    size_t actual_bytes = 0;
    int error_number = 0;
    int close_error_number = 0;
    StaticFileReadErrorCode code = StaticFileReadErrorCode::kReadFailed;
};

using StaticFileReadResult = std::expected<std::string, StaticFileReadError>;

struct StaticFileMetadata
{
    std::string canonical_path;
    size_t file_size = 0;
    std::time_t last_modified = 0;
};

using StaticFileMetadataResult = std::expected<StaticFileMetadata, StaticFileReadError>;
using StaticFileDescriptorResult = std::expected<galay::kernel::FileDescriptor,
                                                 StaticFileReadError>;

class StaticFileSession
{
public:
    StaticFileSession() noexcept = default;
    ~StaticFileSession() = default;

    StaticFileSession(const StaticFileSession&) = delete;
    StaticFileSession& operator=(const StaticFileSession&) = delete;
    StaticFileSession(StaticFileSession&&) noexcept = default;
    StaticFileSession& operator=(StaticFileSession&&) noexcept = default;

    /** Read exactly length bytes at the supplied offset. */
    galay::kernel::Task<StaticFileReadResult> readAt(size_t offset, size_t length);

private:
    friend class StaticFileReader;

#if defined(USE_IOURING)
    galay::async::AsyncFile m_file;
#else
    galay::kernel::FileDescriptor m_file;
#endif
};

using StaticFileSessionResult = std::expected<StaticFileSession, StaticFileReadError>;

const char* staticFileReadErrorName(StaticFileReadErrorCode code) noexcept;

/**
 * @brief Coroutine-facing static-file reader.
 *
 * Every operation is offset based, so chunked and range responses share the
 * same implementation and never perform a synchronous read on the scheduler
 * thread. The returned string owns its bytes until the caller finishes sending
 * them.
 */
class StaticFileReader
{
public:
    /** Open one reusable session without blocking the scheduler. */
    static galay::kernel::Task<StaticFileSessionResult> open(const std::string& filePath);

    /** Read exactly fileSize bytes starting at offset zero. */
    static galay::kernel::Task<StaticFileReadResult> readAll(const std::string& filePath,
                                                             size_t fileSize);

    /** Read exactly length bytes at the supplied file offset. */
    static galay::kernel::Task<StaticFileReadResult> readAt(const std::string& filePath,
                                                            size_t offset,
                                                            size_t length);

    /** Resolve and stat a path on the blocking executor. */
    static galay::kernel::Task<StaticFileMetadataResult> inspect(const std::string& filePath);

    /** Open a descriptor for zero-copy sendfile without blocking the scheduler. */
    static galay::kernel::Task<StaticFileDescriptorResult> openForSendfile(
        const std::string& filePath);
};

} // namespace galay::http

#endif // GALAY_HTTP_STATIC_FILE_READER_H
