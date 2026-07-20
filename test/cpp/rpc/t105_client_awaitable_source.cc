#include <fcntl.h>
#include <unistd.h>

#include <expected>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

enum class ReadError { Open, Read, Close, Path };

std::expected<std::string, ReadError> readFile(std::string_view relative)
{
    std::string path(__FILE__);
    const std::string marker = "/test/cpp/rpc/";
    const size_t marker_pos = path.rfind(marker);
    if (marker_pos == std::string::npos) {
        return std::unexpected(ReadError::Path);
    }
    path.resize(marker_pos);
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
    const auto client = readFile("/src/cpp/galay-rpc/kernel/rpc_client.h");
    const auto awaitable = readFile("/src/cpp/galay-rpc/details/client_awaitable.h");
    const auto implementation = readFile("/src/cpp/galay-rpc/details/client_awaitable.inl");
    if (!client.has_value() || !awaitable.has_value() || !implementation.has_value()) {
        std::cerr << "failed to read RPC client awaitable source boundary\n";
        return 1;
    }

    if (contains(*client, "class RecvRpcResponseChainAwaitable\n") ||
        !contains(*awaitable, "class RecvRpcResponseChainAwaitable")) {
        std::cerr << "RPC client awaitable declaration must live in details/client_awaitable.h\n";
        return 1;
    }
    if (contains(*client, "ExpectedRpcResponseReadState :") ||
        !contains(*implementation, "ExpectedRpcResponseReadState<Strategy>::")) {
        std::cerr << "RPC response read state implementation must live in details/client_awaitable.inl\n";
        return 1;
    }
    if (!contains(*client, "../details/client_awaitable.h")) {
        std::cerr << "RPC client header must include details/client_awaitable.h\n";
        return 1;
    }

    std::cout << "RPC client awaitable source boundary PASS\n";
    return 0;
}
