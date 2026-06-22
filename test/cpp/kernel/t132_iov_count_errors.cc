/**
 * @file t132_iov_count_errors.cc
 * @brief 验证 readv/writev 借用数组 count 越界不 abort，改为可检查错误。
 */

#include <galay/cpp/galay-kernel/async/tcp_socket.h>

#include <array>
#include <iostream>
#include <sys/uio.h>

namespace {

bool check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "[t132] " << message << '\n';
    }
    return condition;
}

bool hasParamInvalid(const std::expected<size_t, galay::kernel::IOError>& result)
{
    return !result && galay::kernel::IOError::contains(result.error().code(), galay::kernel::kParamInvalid);
}

bool invalidReadvCountReturnsError()
{
    galay::async::TcpSocket socket(GHandle::invalid());
    std::array<struct iovec, 1> iovecs{};

    auto awaitable = socket.readv(iovecs, iovecs.size() + 1);
    if (!check(awaitable.await_ready(), "invalid readv count should be immediately ready")) {
        return false;
    }

    return check(hasParamInvalid(awaitable.await_resume()), "invalid readv count should return kParamInvalid");
}

bool invalidWritevCountReturnsError()
{
    galay::async::TcpSocket socket(GHandle::invalid());
    std::array<struct iovec, 1> iovecs{};

    auto awaitable = socket.writev(iovecs, iovecs.size() + 1);
    if (!check(awaitable.await_ready(), "invalid writev count should be immediately ready")) {
        return false;
    }

    return check(hasParamInvalid(awaitable.await_resume()), "invalid writev count should return kParamInvalid");
}

} // namespace

int main()
{
    bool ok = true;
    ok = invalidReadvCountReturnsError() && ok;
    ok = invalidWritevCountReturnsError() && ok;
    return ok ? 0 : 1;
}
