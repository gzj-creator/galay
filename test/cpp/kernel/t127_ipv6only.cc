/**
 * @file t127_ipv6only.cc
 * @brief 用途：验证 HandleOption 可以显式设置 IPV6_V6ONLY。
 * 关键覆盖点：IPv6 listener 可在 bind 前选择 IPv6-only 或 dual-stack 行为。
 * 通过条件：handleIPv6Only(true/false) 成功写入内核 socket option，测试返回 0。
 */

#include <galay/cpp/galay-kernel/common/handle_option.h>
#include <galay/cpp/galay-kernel/async/tcp_socket.h>
#include <galay/cpp/galay-kernel/async/udp_socket.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

#if defined(__linux__) || defined(__APPLE__)
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace galay::kernel;

namespace {

std::string readAll(const std::filesystem::path& path)
{
    std::ifstream input(path);
    if (!input.is_open()) {
        return {};
    }
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

bool contains(const std::string& text, const std::string& needle)
{
    return text.find(needle) != std::string::npos;
}

bool verifySourceSetsDefaultDualStack()
{
    const auto root = std::filesystem::path(GALAY_SOURCE_ROOT);
    const auto tcp_source = root / "galay-kernel" / "async" / "tcp_socket.cc";
    const auto udp_source = root / "galay-kernel" / "async" / "udp_socket.cc";
    const auto tcp_text = readAll(tcp_source);
    const auto udp_text = readAll(udp_source);

    if (!contains(tcp_text, "handleIPv6Only(false)")) {
        std::cerr << "[T127] TcpSocket::openHandle must explicitly disable IPV6_V6ONLY for IPv6 sockets\n";
        return false;
    }
    if (!contains(udp_text, "handleIPv6Only(false)")) {
        std::cerr << "[T127] UdpSocket::openHandle must explicitly disable IPV6_V6ONLY for IPv6 sockets\n";
        return false;
    }
    return true;
}

#if defined(__linux__) || defined(__APPLE__)
bool readIpv6Only(int fd, int& value)
{
    socklen_t value_len = sizeof(value);
    if (::getsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &value, &value_len) != 0) {
        std::cerr << "[T127] getsockopt(IPV6_V6ONLY) failed: " << std::strerror(errno) << "\n";
        return false;
    }
    return true;
}

bool verifyHandleIpv6Only()
{
    const int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
    if (fd < 0) {
        if (errno == EAFNOSUPPORT || errno == EPROTONOSUPPORT) {
            std::cout << "T127-HandleOptionIPv6Only SKIP\n";
            return true;
        }
        std::cerr << "[T127] socket(AF_INET6) failed: " << std::strerror(errno) << "\n";
        return false;
    }

    auto close_fd = [&fd]() {
        if (fd >= 0) {
            (void)::close(fd);
        }
    };

    auto enabled = HandleOption(GHandle{fd}).handleIPv6Only(true);
    if (!enabled) {
        std::cerr << "[T127] handleIPv6Only(true) failed: " << enabled.error().message() << "\n";
        close_fd();
        return false;
    }
    int value = 0;
    if (!readIpv6Only(fd, value)) {
        close_fd();
        return false;
    }
    if (value == 0) {
        std::cerr << "[T127] expected IPV6_V6ONLY to be enabled\n";
        close_fd();
        return false;
    }

    auto disabled = HandleOption(GHandle{fd}).handleIPv6Only(false);
    if (!disabled) {
        std::cerr << "[T127] handleIPv6Only(false) failed: " << disabled.error().message() << "\n";
        close_fd();
        return false;
    }
    if (!readIpv6Only(fd, value)) {
        close_fd();
        return false;
    }
    if (value != 0) {
        std::cerr << "[T127] expected IPV6_V6ONLY to be disabled\n";
        close_fd();
        return false;
    }

    auto invalid = HandleOption(GHandle{-1}).handleIPv6Only(true);
    if (invalid) {
        std::cerr << "[T127] invalid handle unexpectedly accepted IPV6_V6ONLY\n";
        close_fd();
        return false;
    }

    close_fd();
    return true;
}

bool verifySocketFactoriesDefaultToDualStack()
{
    auto tcp_socket = galay::async::TcpSocket::create(IPType::IPV6);
    if (!tcp_socket) {
        std::cerr << "[T127] TcpSocket::create(IPV6) failed: " << tcp_socket.error().message() << "\n";
        return false;
    }

    int value = 1;
    if (!readIpv6Only(tcp_socket->handle().fd, value)) {
        return false;
    }
    if (value != 0) {
        std::cerr << "[T127] TcpSocket::create(IPV6) must default to dual-stack\n";
        return false;
    }

    auto tcp_only = tcp_socket->option().handleIPv6Only(true);
    if (!tcp_only) {
        std::cerr << "[T127] TcpSocket explicit IPv6-only failed: " << tcp_only.error().message() << "\n";
        return false;
    }
    if (!readIpv6Only(tcp_socket->handle().fd, value) || value == 0) {
        std::cerr << "[T127] TcpSocket explicit IPv6-only override did not take effect\n";
        return false;
    }

    auto udp_socket = galay::async::UdpSocket::create(IPType::IPV6);
    if (!udp_socket) {
        std::cerr << "[T127] UdpSocket::create(IPV6) failed: " << udp_socket.error().message() << "\n";
        return false;
    }

    value = 1;
    if (!readIpv6Only(udp_socket->handle().fd, value)) {
        return false;
    }
    if (value != 0) {
        std::cerr << "[T127] UdpSocket::create(IPV6) must default to dual-stack\n";
        return false;
    }

    auto udp_only = udp_socket->option().handleIPv6Only(true);
    if (!udp_only) {
        std::cerr << "[T127] UdpSocket explicit IPv6-only failed: " << udp_only.error().message() << "\n";
        return false;
    }
    if (!readIpv6Only(udp_socket->handle().fd, value) || value == 0) {
        std::cerr << "[T127] UdpSocket explicit IPv6-only override did not take effect\n";
        return false;
    }

    return true;
}
#else
bool verifyHandleIpv6Only()
{
    std::cout << "T127-HandleOptionIPv6Only SKIP\n";
    return true;
}

bool verifySocketFactoriesDefaultToDualStack()
{
    return true;
}
#endif

}  // namespace

int main()
{
    if (!verifySourceSetsDefaultDualStack()) {
        return 1;
    }
    if (!verifyHandleIpv6Only()) {
        return 2;
    }
    if (!verifySocketFactoriesDefaultToDualStack()) {
        return 3;
    }

#if defined(__linux__) || defined(__APPLE__)
    std::cout << "T127-HandleOptionIPv6Only PASS\n";
#endif
    return 0;
}
