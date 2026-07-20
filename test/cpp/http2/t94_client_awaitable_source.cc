#include <fcntl.h>
#include <unistd.h>

#include <expected>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

enum class ReadError {
    Open,
    Read,
    Close,
    Path,
};

std::expected<std::string, ReadError> repoRoot()
{
    std::string path(__FILE__);
    const std::string marker = "/test/cpp/http2/";
    const size_t marker_pos = path.rfind(marker);
    if (marker_pos == std::string::npos) {
        return std::unexpected(ReadError::Path);
    }
    path.resize(marker_pos);
    return path;
}

std::expected<std::string, ReadError> readFile(std::string_view relative)
{
    auto root = repoRoot();
    if (!root.has_value()) {
        return std::unexpected(root.error());
    }
    std::string path = std::move(*root);
    std::string& path_result = path.append(relative);
    if (&path_result != &path) {
        return std::unexpected(ReadError::Path);
    }

    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return std::unexpected(ReadError::Open);
    }

    std::string content;
    std::vector<char> buffer(64 * 1024);
    for (;;) {
        const ssize_t count = ::read(fd, buffer.data(), buffer.size());
        if (count < 0) {
            const int close_result = ::close(fd);
            if (close_result != 0) {
                std::cerr << "close after read failure also failed\n";
            }
            return std::unexpected(ReadError::Read);
        }
        if (count == 0) {
            break;
        }
        std::string& append_result = content.append(buffer.data(), static_cast<size_t>(count));
        if (&append_result != &content) {
            const int close_result = ::close(fd);
            if (close_result != 0) {
                std::cerr << "close after append failure also failed\n";
            }
            return std::unexpected(ReadError::Read);
        }
    }

    if (::close(fd) != 0) {
        return std::unexpected(ReadError::Close);
    }
    return content;
}

bool contains(std::string_view text, std::string_view needle)
{
    return text.find(needle) != std::string_view::npos;
}

} // namespace

int main()
{
    const auto h2 = readFile("/src/cpp/galay-http2/client/h2_client.h");
    const auto h2c = readFile("/src/cpp/galay-http2/client/h2c_client.h");
    const auto h2_awaitable = readFile("/src/cpp/galay-http2/details/h2_client_awaitable.h");
    const auto h2_implementation = readFile("/src/cpp/galay-http2/details/h2_client_awaitable.inl");
    const auto h2c_awaitable = readFile("/src/cpp/galay-http2/details/h2c_client_awaitable.h");
    const auto h2c_implementation = readFile("/src/cpp/galay-http2/details/h2c_client_awaitable.inl");
    if (!h2.has_value() || !h2c.has_value() || !h2_awaitable.has_value() ||
        !h2_implementation.has_value() || !h2c_awaitable.has_value() ||
        !h2c_implementation.has_value()) {
        std::cerr << "failed to read HTTP/2 client awaitable source boundary\n";
        return 1;
    }

    for (const auto definition : {
             "class CaptureSchedulerAwaitable",
         }) {
        if (contains(*h2, definition) ||
            (!contains(*h2_awaitable, definition) &&
             !contains(*h2_implementation, definition))) {
            std::cerr << "HTTP/2 TLS client awaitable implementation must live in details: "
                      << definition << "\n";
            return 1;
        }
    }

    for (const auto definition : {
             "class H2cUpgradeAwaitable\n    :",
         }) {
        if (contains(*h2c, definition) ||
            (!contains(*h2c_awaitable, definition) &&
             !contains(*h2c_implementation, definition))) {
            std::cerr << "HTTP/2 cleartext client awaitable implementation must live in details: "
                      << definition << "\n";
            return 1;
        }
    }

    if (!contains(*h2, "../details/h2_client_awaitable.h") ||
        !contains(*h2c, "../details/h2c_client_awaitable.h")) {
        std::cerr << "HTTP/2 client headers must expose awaitables through details headers\n";
        return 1;
    }

    std::cout << "HTTP/2 client awaitable source boundary PASS\n";
    return 0;
}
