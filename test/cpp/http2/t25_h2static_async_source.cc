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
    kPath,
};

std::expected<std::string, ReadError> repo_root()
{
    std::string path(__FILE__);
    const std::string marker = "/test/cpp/http2/";
    const size_t marker_pos = path.rfind(marker);
    if (marker_pos == std::string::npos) {
        return std::unexpected(ReadError::kPath);
    }
    path.resize(marker_pos);
    return path;
}

std::expected<std::string, ReadError> read_file(const std::string& path)
{
    const int fd = ::open(path.c_str(), O_RDONLY);
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

std::expected<std::string, ReadError> read_repo_file(std::string_view relative)
{
    auto root = repo_root();
    if (!root.has_value()) {
        return std::unexpected(root.error());
    }
    std::string path = *root;
    std::string& appended = path.append(relative.data(), relative.size());
    if (&appended != &path) {
        return std::unexpected(ReadError::kPath);
    }
    return read_file(path);
}

int require_contains(std::string_view haystack, std::string_view needle, const char* message)
{
    const size_t found = haystack.find(needle);
    if (found == std::string_view::npos) {
        std::cerr << message << "\n";
        return 1;
    }
    return 0;
}

int require_not_contains(std::string_view haystack, std::string_view needle, const char* message)
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
    auto stream_manager = read_repo_file("/src/cpp/galay-http2/kernel/stream_manager.h");
    if (!stream_manager.has_value()) {
        std::cerr << "failed to read HTTP/2 stream_manager.h\n";
        return 1;
    }
    auto static_file = read_repo_file("/src/cpp/galay-http2/server/h2_static_file.cc");
    if (!static_file.has_value()) {
        std::cerr << "failed to read HTTP/2 h2_static_file.cc\n";
        return 1;
    }

    if (const int rc = require_contains(*stream_manager,
                                       "spawn_blocking",
                                       "HTTP/2 static file body read must use runtime blocking pool")) {
        return rc;
    }
    if (const int rc = require_contains(*stream_manager,
                                       "read_static_file_chunks_blocking",
                                       "HTTP/2 static file body read must be isolated outside the event-loop path")) {
        return rc;
    }
    if (const int rc = require_not_contains(*stream_manager,
                                          "std::ifstream",
                                          "HTTP/2 stream manager must not synchronously read files with ifstream")) {
        return rc;
    }
    if (const int rc = require_not_contains(*static_file,
                                          "std::ifstream",
                                          "HTTP/2 static cache miss path must not synchronously read file bodies")) {
        return rc;
    }
    if (const int rc = require_not_contains(*static_file,
                                          "read_small_file",
                                          "HTTP/2 small file cache miss must not synchronously read body")) {
        return rc;
    }
    if (const int rc = require_not_contains(*static_file,
                                          "read_file_range",
                                          "HTTP/2 range lookup must not synchronously read body")) {
        return rc;
    }

    std::cout << "T25-H2StaticAsyncSource PASS\n";
    return 0;
}
