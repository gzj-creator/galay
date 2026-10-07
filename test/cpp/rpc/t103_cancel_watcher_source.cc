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

std::expected<std::string, ReadError> channel_source_path()
{
    std::string path(__FILE__);
    const std::string marker = "/test/cpp/rpc/";
    const size_t marker_pos = path.rfind(marker);
    if (marker_pos == std::string::npos) {
        return std::unexpected(ReadError::kOpen);
    }
    path.resize(marker_pos);
    std::string& appended = path.append("/src/cpp/galay-rpc/kernel/rpc_channel.h");
    if (&appended != &path) {
        return std::unexpected(ReadError::kOpen);
    }
    return path;
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
    auto channel_path = channel_source_path();
    if (!channel_path.has_value()) {
        std::cerr << "failed to resolve rpc_channel.h source path\n";
        return 1;
    }

    auto source = read_file(*channel_path);
    if (!source.has_value()) {
        std::cerr << "failed to read " << *channel_path << "\n";
        return 1;
    }

    if (const int rc = require_contains(*source,
                                       "register_callback",
                                       "RPC cancellation token must notify pending calls without polling")) {
        return rc;
    }
    if (const int rc = require_contains(*source,
                                       "cancellation_registration",
                                       "RPC caller coroutine must own its cancellation registration")) {
        return rc;
    }
    if (const int rc = require_not_contains(*source,
                                          "cancel_watch_loop",
                                          "RPC channel must not keep per-call cancellation watcher coroutine")) {
        return rc;
    }
    if (const int rc = require_not_contains(*source,
                                          "cancel_sweep_loop",
                                          "RPC channel must not poll cancellation from a sweep coroutine")) {
        return rc;
    }
    if (const int rc = require_not_contains(*source,
                                          "fail_cancelled_pending",
                                          "RPC channel must not scan the pending map for cancellation")) {
        return rc;
    }
    if (const int rc = require_not_contains(*source,
                                          "schedule_task(scheduler_for_cancel",
                                          "RPC channel must not schedule one cancel watcher per call")) {
        return rc;
    }

    std::cout << "T103-CancelWatcherSource PASS\n";
    return 0;
}
