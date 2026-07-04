/**
 * @file t139_sigpipe_policy.cc
 * @brief 验证 kernel 不修改全局 SIGPIPE，socket 写路径使用局部 SIGPIPE 抑制。
 */

#include <galay/cpp/galay-kernel/async/tcp_socket.h>
#include <galay/cpp/galay-kernel/core/io_handlers.hpp>
#include <galay/cpp/galay-kernel/core/runtime.h>

#include <csignal>
#include <iostream>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

namespace {

volatile sig_atomic_t g_sigpipe_seen = 0;

void sigpipeHandler(int)
{
    g_sigpipe_seen = 1;
}

bool check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "[t139] " << message << '\n';
    }
    return condition;
}

bool installSigpipeHandler(void (*handler)(int), struct sigaction* previous)
{
    struct sigaction action{};
    action.sa_handler = handler;
    if (sigemptyset(&action.sa_mask) != 0) {
        return false;
    }
    action.sa_flags = 0;
    return ::sigaction(SIGPIPE, &action, previous) == 0;
}

bool restoreSigpipeHandler(const struct sigaction& previous)
{
    return ::sigaction(SIGPIPE, &previous, nullptr) == 0;
}

bool runtimeConstructorDoesNotChangeSigpipe()
{
    struct sigaction previous{};
    if (!check(installSigpipeHandler(SIG_DFL, &previous), "failed to install default SIGPIPE handler")) {
        return false;
    }

    bool ok = true;
    {
        galay::kernel::Runtime runtime;
        struct sigaction current{};
        ok = check(::sigaction(SIGPIPE, nullptr, &current) == 0, "failed to read SIGPIPE handler") && ok;
        ok = check(current.sa_handler == SIG_DFL, "Runtime constructor must not ignore SIGPIPE globally") && ok;
    }

    ok = check(restoreSigpipeHandler(previous), "failed to restore SIGPIPE handler") && ok;
    return ok;
}

#if defined(__linux__)
bool makeBrokenSocketPair(int fds[2])
{
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        return false;
    }
    if (::close(fds[1]) != 0) {
        const int close_errno = errno;
        const int cleanup_result = ::close(fds[0]);
        const int cleanup_errno = errno;
        fds[0] = -1;
        fds[1] = -1;
        errno = (cleanup_result == 0) ? close_errno : cleanup_errno;
        return false;
    }
    fds[1] = -1;
    return true;
}

bool closeFd(int& fd)
{
    if (fd < 0) {
        return true;
    }
    const int close_result = ::close(fd);
    fd = -1;
    return close_result == 0;
}

bool linuxSendDoesNotDeliverSigpipe()
{
    struct sigaction previous{};
    if (!check(installSigpipeHandler(sigpipeHandler, &previous), "failed to install SIGPIPE test handler")) {
        return false;
    }

    int fds[2]{-1, -1};
    bool ok = check(makeBrokenSocketPair(fds), "failed to create broken socket pair");
    if (ok) {
        g_sigpipe_seen = 0;
        const char payload[] = "x";
        auto result = galay::kernel::io::handleSend(GHandle{.fd = fds[0]}, payload, sizeof(payload));
        ok = check(!result, "send to closed peer should fail") && ok;
        ok = check(g_sigpipe_seen == 0, "send path must pass MSG_NOSIGNAL") && ok;
    }

    ok = check(closeFd(fds[0]), "failed to close send test fd") && ok;
    ok = check(restoreSigpipeHandler(previous), "failed to restore SIGPIPE handler") && ok;
    return ok;
}

bool linuxWritevDoesNotDeliverSigpipe()
{
    struct sigaction previous{};
    if (!check(installSigpipeHandler(sigpipeHandler, &previous), "failed to install SIGPIPE test handler")) {
        return false;
    }

    int fds[2]{-1, -1};
    bool ok = check(makeBrokenSocketPair(fds), "failed to create broken socket pair");
    if (ok) {
        g_sigpipe_seen = 0;
        char payload[] = "xy";
        struct iovec iovecs[2]{
            {.iov_base = payload, .iov_len = 1},
            {.iov_base = payload + 1, .iov_len = 1},
        };
        auto result = galay::kernel::io::handleWritev(GHandle{.fd = fds[0]}, iovecs, 2);
        ok = check(!result, "writev to closed peer should fail") && ok;
        ok = check(g_sigpipe_seen == 0, "writev path must use sendmsg with MSG_NOSIGNAL") && ok;
    }

    ok = check(closeFd(fds[0]), "failed to close writev test fd") && ok;
    ok = check(restoreSigpipeHandler(previous), "failed to restore SIGPIPE handler") && ok;
    return ok;
}
#endif

#if defined(SO_NOSIGPIPE)
bool tcpCreateEnablesNoSigpipe()
{
    auto socket = galay::async::TcpSocket::create(galay::kernel::IPType::IPV4);
    if (!check(socket.has_value(), "failed to create TCP socket")) {
        return false;
    }

    int enabled = 0;
    socklen_t length = sizeof(enabled);
    bool ok = check(::getsockopt(socket->handle().fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, &length) == 0,
                    "failed to read SO_NOSIGPIPE") &&
              check(enabled == 1, "TCP socket should enable SO_NOSIGPIPE when the platform supports it");
    return ok;
}
#endif

} // namespace

int main()
{
    bool ok = true;
    ok = runtimeConstructorDoesNotChangeSigpipe() && ok;
#if defined(__linux__)
    ok = linuxSendDoesNotDeliverSigpipe() && ok;
    ok = linuxWritevDoesNotDeliverSigpipe() && ok;
#endif
#if defined(SO_NOSIGPIPE)
    ok = tcpCreateEnablesNoSigpipe() && ok;
#endif
    return ok ? 0 : 1;
}
