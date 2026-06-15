#include <cassert>
#include <cstring>
#include <iostream>

#include "galay-kernel/common/handle_option.h"

#if defined(__APPLE__) || defined(__linux__)
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

int main() {
#if defined(__APPLE__) || defined(__linux__)
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "[T88] failed to create socket: " << std::strerror(errno) << "\n";
        return 1;
    }

    auto result = galay::kernel::HandleOption(GHandle{fd}).handleTcpNoDelay();
    if (!result) {
        std::cerr << "[T88] handleTcpNoDelay failed: " << result.error().message() << "\n";
        ::close(fd);
        return 1;
    }

    int value = 0;
    socklen_t value_len = sizeof(value);
    if (::getsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &value, &value_len) != 0) {
        std::cerr << "[T88] getsockopt(TCP_NODELAY) failed: " << std::strerror(errno) << "\n";
        ::close(fd);
        return 1;
    }

    ::close(fd);
    if (value == 0) {
        std::cerr << "[T88] TCP_NODELAY expected non-zero, got " << value << "\n";
        return 1;
    }
#endif

    std::cout << "T88-HandleOptionTcpNoDelay PASS\n";
    return 0;
}
