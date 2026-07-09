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

std::expected<std::string, ReadError> repoRoot()
{
    std::string path(__FILE__);
    const std::string marker = "/test/cpp/http/";
    const size_t marker_pos = path.rfind(marker);
    if (marker_pos == std::string::npos) {
        return std::unexpected(ReadError::kPath);
    }
    path.resize(marker_pos);
    return path;
}

std::expected<std::string, ReadError> readFile(const std::string& path)
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

std::expected<std::string, ReadError> repoFile(std::string_view relative_path)
{
    auto root = repoRoot();
    if (!root.has_value()) {
        return std::unexpected(root.error());
    }
    std::string path = *root;
    std::string& slash_appended = path.append("/");
    if (&slash_appended != &path) {
        return std::unexpected(ReadError::kPath);
    }
    std::string& file_appended = path.append(relative_path.data(), relative_path.size());
    if (&file_appended != &path) {
        return std::unexpected(ReadError::kPath);
    }
    return readFile(path);
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
    auto writer = repoFile("src/cpp/galay-http/kernel/http_writer.h");
    if (!writer.has_value()) {
        std::cerr << "failed to read http_writer.h\n";
        return 1;
    }

    if (const int rc = requireContains(*writer,
                                       "std::array<iovec, 2>",
                                       "HttpWriter TCP layout must use fixed iovec storage")) {
        return rc;
    }
    if (const int rc = requireContains(*writer,
                                       "m_writev_cursor.reserve(2)",
                                       "HttpWriter must reserve cursor storage once")) {
        return rc;
    }
    if (const int rc = requireNotContains(*writer,
                                          "std::vector<iovec> iovecs;",
                                          "HttpWriter hot path must not construct local vector<iovec>")) {
        return rc;
    }
    if (const int rc = requireNotContains(*writer,
                                          "m_writev_cursor.reset(std::vector<iovec>{})",
                                          "HttpWriter cleanup must clear the writev cursor without a temporary vector")) {
        return rc;
    }

    std::cout << "T89-WriterBufferSource PASS\n";
    return 0;
}
