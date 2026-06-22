/**
 * @file t30_iovguard.cc
 * @brief 用途：验证 `readv/writev` 借用数组接口对元素数量边界的保护逻辑。
 * 关键覆盖点：数组计数上限、非法计数防御、边界输入下的可检查错误返回。
 * 通过条件：越界 count 不终止进程，立即返回 kParamInvalid。
 */

#include <array>
#include <expected>
#include <iostream>
#include <sys/uio.h>

#include <galay/cpp/galay-kernel/async/tcp_socket.h>

namespace {

bool check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "[t30] " << message << '\n';
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
    std::array<struct iovec, 2> iovecs{};

    auto awaitable = socket.readv(iovecs, iovecs.size() + 1);
    if (!check(awaitable.await_ready(), "invalid readv count should be immediately ready")) {
        return false;
    }

    return check(hasParamInvalid(awaitable.await_resume()), "invalid readv count should return kParamInvalid");
}

bool invalidWritevCountReturnsError()
{
    galay::async::TcpSocket socket(GHandle::invalid());
    char header[] = "guard";
    char body[] = "check";
    std::array<struct iovec, 2> iovecs{};
    iovecs[0].iov_base = header;
    iovecs[0].iov_len = sizeof(header) - 1;
    iovecs[1].iov_base = body;
    iovecs[1].iov_len = sizeof(body) - 1;

    auto awaitable = socket.writev(iovecs, iovecs.size() + 1);
    if (!check(awaitable.await_ready(), "invalid writev count should be immediately ready")) {
        return false;
    }

    return check(hasParamInvalid(awaitable.await_resume()), "invalid writev count should return kParamInvalid");
}

}  // namespace

int main()
{
    bool ok = true;
    ok = invalidReadvCountReturnsError() && ok;
    ok = invalidWritevCountReturnsError() && ok;
    return ok ? 0 : 1;
}
