#include <fcntl.h>
#include <unistd.h>

#include <expected>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

enum class ReadError {
    kOpen,
    kRead,
    kClose,
};

std::expected<std::string, ReadError> readFile(const char* path)
{
    const int fd = ::open(path, O_RDONLY);
    if (fd < 0) {
        return std::unexpected(ReadError::kOpen);
    }

    std::string content;
    std::vector<char> buffer(64 * 1024);
    while (true) {
        const ssize_t bytes_read = ::read(fd, buffer.data(), buffer.size());
        if (bytes_read < 0) {
            const int close_result = ::close(fd);
            if (close_result != 0) {
                std::cerr << "close after read failure also failed\n";
            }
            return std::unexpected(ReadError::kRead);
        }
        if (bytes_read == 0) {
            break;
        }
        std::string& appended = content.append(buffer.data(), static_cast<size_t>(bytes_read));
        if (&appended != &content) {
            const int close_result = ::close(fd);
            if (close_result != 0) {
                std::cerr << "close after append failure also failed\n";
            }
            return std::unexpected(ReadError::kRead);
        }
    }

    const int close_result = ::close(fd);
    if (close_result != 0) {
        return std::unexpected(ReadError::kClose);
    }
    return content;
}

std::expected<std::string_view, const char*> extractMemoryBranch(std::string_view source)
{
    const std::string_view begin_marker = "case FileTransferMode::MEMORY:";
    const size_t begin = source.find(begin_marker);
    if (begin == std::string_view::npos) {
        return std::unexpected("MEMORY branch not found");
    }

    const std::string_view end_marker = "case FileTransferMode::CHUNK:";
    const size_t end = source.find(end_marker, begin + begin_marker.size());
    if (end == std::string_view::npos) {
        return std::unexpected("CHUNK branch not found after MEMORY branch");
    }

    return source.substr(begin, end - begin);
}

std::expected<std::string_view, const char*> extractRangeFunction(std::string_view source,
                                                                  std::string_view begin_marker,
                                                                  std::string_view end_marker)
{
    const size_t begin = source.find(begin_marker);
    if (begin == std::string_view::npos) {
        return std::unexpected("range function not found");
    }

    if (end_marker.empty()) {
        return source.substr(begin);
    }

    const size_t end = source.find(end_marker, begin + begin_marker.size());
    if (end == std::string_view::npos) {
        return std::unexpected("next range function not found");
    }

    return source.substr(begin, end - begin);
}

int requireContains(std::string_view haystack, std::string_view needle, const char* message)
{
    const size_t found = haystack.find(needle);
    if (found == std::string_view::npos) {
        std::cerr << message << "\n";
        return 1;
    }
    return 0;
}

int requireNotContains(std::string_view haystack, std::string_view needle, const char* message)
{
    const size_t found = haystack.find(needle);
    if (found != std::string_view::npos) {
        std::cerr << message << "\n";
        return 1;
    }
    return 0;
}

} // namespace

int main()
{
    const char* router_path = "src/cpp/galay-http/server/http_router.cc";
    auto source = readFile(router_path);
    if (!source.has_value()) {
        std::cerr << "failed to read " << router_path << "\n";
        return 1;
    }

    auto memory_branch = extractMemoryBranch(*source);
    if (!memory_branch.has_value()) {
        std::cerr << memory_branch.error() << "\n";
        return 1;
    }

    if (const int rc = requireContains(*memory_branch,
                                       "StaticFileReader",
                                       "HTTP/1 MEMORY static file path must use StaticFileReader")) {
        return rc;
    }
    if (const int rc = requireNotContains(*memory_branch,
                                          "spawnBlocking",
                                          "HTTP/1 MEMORY static file path must not assemble spawnBlocking directly")) {
        return rc;
    }
    if (const int rc = requireNotContains(*memory_branch,
                                          "AsyncWaiter",
                                          "HTTP/1 MEMORY static file path must not assemble AsyncWaiter directly")) {
        return rc;
    }
    if (const int rc = requireContains(*memory_branch,
                                       "co_await",
                                       "HTTP/1 MEMORY static file path must await completion instead of blocking")) {
        return rc;
    }
    if (const int rc = requireNotContains(*memory_branch,
                                          "std::ifstream",
                                          "HTTP/1 MEMORY static file coroutine branch must not construct ifstream directly")) {
        return rc;
    }
    if (const int rc = requireNotContains(*memory_branch,
                                          ".join(",
                                          "HTTP/1 MEMORY static file coroutine branch must not join blocking work")) {
        return rc;
    }

    auto chunk_branch = extractRangeFunction(*source,
                                             "case FileTransferMode::CHUNK:",
                                             "case FileTransferMode::SENDFILE:");
    if (!chunk_branch.has_value()) {
        std::cerr << chunk_branch.error() << "\n";
        return 1;
    }
    if (const int rc = requireContains(*chunk_branch,
                                       "StaticFileReader",
                                       "HTTP/1 CHUNK static file path must use StaticFileReader")) {
        return rc;
    }
    if (const int rc = requireNotContains(*chunk_branch,
                                          "read(",
                                          "HTTP/1 CHUNK static file path must not call read() directly")) {
        return rc;
    }
    if (const int rc = requireNotContains(*chunk_branch,
                                          ".open(",
                                          "HTTP/1 CHUNK static file path must not open files directly")) {
        return rc;
    }

    auto single_range = extractRangeFunction(*source,
                                             "Task<void> HttpRouter::sendSingleRange",
                                             "Task<void> HttpRouter::sendMultipleRanges");
    if (!single_range.has_value()) {
        std::cerr << single_range.error() << "\n";
        return 1;
    }
    if (const int rc = requireContains(*single_range,
                                       "StaticFileReader",
                                       "HTTP/1 single-range path must use StaticFileReader")) {
        return rc;
    }
    if (const int rc = requireNotContains(*single_range,
                                          "read(",
                                          "HTTP/1 single-range path must not call read() directly")) {
        return rc;
    }
    if (const int rc = requireNotContains(*single_range,
                                          "lseek(",
                                          "HTTP/1 single-range path must not seek files directly")) {
        return rc;
    }
    if (const int rc = requireNotContains(*single_range,
                                          ".open(",
                                          "HTTP/1 single-range path must not open files directly")) {
        return rc;
    }

    auto multiple_ranges = extractRangeFunction(*source,
                                                "Task<void> HttpRouter::sendMultipleRanges",
                                                "");
    if (!multiple_ranges.has_value()) {
        std::cerr << multiple_ranges.error() << "\n";
        return 1;
    }
    if (const int rc = requireContains(*multiple_ranges,
                                       "StaticFileReader",
                                       "HTTP/1 multi-range path must use StaticFileReader")) {
        return rc;
    }
    if (const int rc = requireNotContains(*multiple_ranges,
                                          "read(",
                                          "HTTP/1 multi-range path must not call read() directly")) {
        return rc;
    }
    if (const int rc = requireNotContains(*multiple_ranges,
                                          "lseek(",
                                          "HTTP/1 multi-range path must not seek files directly")) {
        return rc;
    }
    if (const int rc = requireNotContains(*multiple_ranges,
                                          ".open(",
                                          "HTTP/1 multi-range path must not open files directly")) {
        return rc;
    }

    std::cout << "T86-StaticMemoryAsyncSource PASS\n";
    return 0;
}
