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
    const char* server_path = "src/cpp/galay-http/server/http_server.h";
    auto source = readFile(server_path);
    if (!source.has_value()) {
        std::cerr << "failed to read " << server_path << "\n";
        return 1;
    }

    if (const int rc = requireContains(*source,
                                       "scheduleRuntimeTask",
                                       "HTTP server root tasks must use a runtime-binding scheduler helper")) {
        return rc;
    }
    if (const int rc = requireContains(*source,
                                       "detail::setTaskRuntime",
                                       "HTTP server root task helper must bind m_runtime into Task state")) {
        return rc;
    }
    if (const int rc = requireNotContains(*source,
                                          "\n        m_runtime.start();",
                                          "HTTP server must check Runtime::start() return value")) {
        return rc;
    }
    if (const int rc = requireNotContains(*source,
                                          "scheduleTask(scheduler, serverLoop",
                                          "HTTP server must check and runtime-bind serverLoop scheduling")) {
        return rc;
    }
    if (const int rc = requireNotContains(*source,
                                          "scheduleTask(scheduler, m_handler",
                                          "HTTP server must check and runtime-bind connection handler scheduling")) {
        return rc;
    }
    if (const int rc = requireNotContains(*source,
                                          "scheduleTask(target_scheduler",
                                          "HTTPS server must check and runtime-bind TLS handler scheduling")) {
        return rc;
    }

    std::cout << "T87-ServerRuntimeBindingSource PASS\n";
    return 0;
}
