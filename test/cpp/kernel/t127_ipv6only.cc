/**
 * @file t127_ipv6only.cc
 * @brief 用途：验证 HandleOption 可以显式设置 IPV6_V6ONLY。
 * 关键覆盖点：IPv6 listener 可在 bind 前选择 IPv6-only 或 dual-stack 行为。
 * 通过条件：handle_ipv6_only(true/false) 成功写入内核 socket option，测试返回 0。
 */

#include <galay/cpp/galay-kernel/common/handle_option.h>
#include <galay/cpp/galay-kernel/async/async_tcp.h>
#include <galay/cpp/galay-kernel/async/async_udp.h>

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

std::string read_all(const std::filesystem::path& path)
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

bool verify_source_sets_default_dual_stack()
{
    const auto root = std::filesystem::path(GALAY_SOURCE_ROOT);
    const auto tcp_source = root / "galay-kernel" / "async" / "async_tcp.cc";
    const auto udp_source = root / "galay-kernel" / "async" / "async_udp.cc";
    const auto tcp_text = read_all(tcp_source);
    const auto udp_text = read_all(udp_source);

    if (!contains(tcp_text, "handle_ipv6_only(false)")) {
        std::cerr << "[T127] AsyncTcpSocket::open_handle must explicitly disable IPV6_V6ONLY for IPv6 sockets\n";
        return false;
    }
    if (!contains(udp_text, "handle_ipv6_only(false)")) {
        std::cerr << "[T127] AsyncUdpSocket::open_handle must explicitly disable IPV6_V6ONLY for IPv6 sockets\n";
        return false;
    }
    return true;
}

#if defined(__linux__) || defined(__APPLE__)
bool read_ipv6_only(int fd, int& value)
{
    socklen_t value_len = sizeof(value);
    if (::getsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &value, &value_len) != 0) {
        std::cerr << "[T127] getsockopt(IPV6_V6ONLY) failed: " << std::strerror(errno) << "\n";
        return false;
    }
    return true;
}

bool verify_handle_ipv6_only()
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

    auto enabled = HandleOption(GHandle{fd}).handle_ipv6_only(true);
    if (!enabled) {
        std::cerr << "[T127] handle_ipv6_only(true) failed: " << enabled.error().message() << "\n";
        close_fd();
        return false;
    }
    int value = 0;
    if (!read_ipv6_only(fd, value)) {
        close_fd();
        return false;
    }
    if (value == 0) {
        std::cerr << "[T127] expected IPV6_V6ONLY to be enabled\n";
        close_fd();
        return false;
    }

    auto disabled = HandleOption(GHandle{fd}).handle_ipv6_only(false);
    if (!disabled) {
        std::cerr << "[T127] handle_ipv6_only(false) failed: " << disabled.error().message() << "\n";
        close_fd();
        return false;
    }
    if (!read_ipv6_only(fd, value)) {
        close_fd();
        return false;
    }
    if (value != 0) {
        std::cerr << "[T127] expected IPV6_V6ONLY to be disabled\n";
        close_fd();
        return false;
    }

    auto invalid = HandleOption(GHandle{-1}).handle_ipv6_only(true);
    if (invalid) {
        std::cerr << "[T127] invalid handle unexpectedly accepted IPV6_V6ONLY\n";
        close_fd();
        return false;
    }

    close_fd();
    return true;
}

bool verify_socket_factories_default_to_dual_stack()
{
    auto tcp_socket = galay::async::AsyncTcpSocket::create(IPType::IPV6);
    if (!tcp_socket) {
        std::cerr << "[T127] AsyncTcpSocket::create(IPV6) failed: " << tcp_socket.error().message() << "\n";
        return false;
    }

    int value = 1;
    if (!read_ipv6_only(tcp_socket->handle().fd, value)) {
        return false;
    }
    if (value != 0) {
        std::cerr << "[T127] AsyncTcpSocket::create(IPV6) must default to dual-stack\n";
        return false;
    }

    auto tcp_only = tcp_socket->option().handle_ipv6_only(true);
    if (!tcp_only) {
        std::cerr << "[T127] AsyncTcpSocket explicit IPv6-only failed: " << tcp_only.error().message() << "\n";
        return false;
    }
    if (!read_ipv6_only(tcp_socket->handle().fd, value) || value == 0) {
        std::cerr << "[T127] AsyncTcpSocket explicit IPv6-only override did not take effect\n";
        return false;
    }

    auto udp_socket = galay::async::AsyncUdpSocket::create(IPType::IPV6);
    if (!udp_socket) {
        std::cerr << "[T127] AsyncUdpSocket::create(IPV6) failed: " << udp_socket.error().message() << "\n";
        return false;
    }

    value = 1;
    if (!read_ipv6_only(udp_socket->handle().fd, value)) {
        return false;
    }
    if (value != 0) {
        std::cerr << "[T127] AsyncUdpSocket::create(IPV6) must default to dual-stack\n";
        return false;
    }

    auto udp_only = udp_socket->option().handle_ipv6_only(true);
    if (!udp_only) {
        std::cerr << "[T127] AsyncUdpSocket explicit IPv6-only failed: " << udp_only.error().message() << "\n";
        return false;
    }
    if (!read_ipv6_only(udp_socket->handle().fd, value) || value == 0) {
        std::cerr << "[T127] AsyncUdpSocket explicit IPv6-only override did not take effect\n";
        return false;
    }

    return true;
}
#else
bool verify_handle_ipv6_only()
{
    std::cout << "T127-HandleOptionIPv6Only SKIP\n";
    return true;
}

bool verify_socket_factories_default_to_dual_stack()
{
    return true;
}
#endif

}  // namespace

int main()
{
    if (!verify_source_sets_default_dual_stack()) {
        return 1;
    }
    if (!verify_handle_ipv6_only()) {
        return 2;
    }
    if (!verify_socket_factories_default_to_dual_stack()) {
        return 3;
    }

#if defined(__linux__) || defined(__APPLE__)
    std::cout << "T127-HandleOptionIPv6Only PASS\n";
#endif
    return 0;
}
