/**
 * @file t30_iovguard.cc
 * @brief 用途：验证 `readv/writev` 借用数组接口对元素数量边界的保护逻辑。
 * 关键覆盖点：`std::array` 与 C 数组的零计数、满容量、越界和极大 count。
 * 通过条件：合法 count 不会被构造阶段拒绝；非法 count 立即返回 kParamInvalid。
 */

#include <array>
#include <expected>
#include <iostream>
#include <limits>
#include <string>
#include <sys/uio.h>

#include <galay/cpp/galay-kernel/async/async_tcp.h>

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

template <typename Iovecs>
bool readvAcceptsCount(galay::async::AsyncTcpSocket& socket,
                       Iovecs& iovecs,
                       size_t count,
                       const std::string& label)
{
    auto awaitable = socket.readv(iovecs, count);
    return check(!awaitable.await_ready(), (label + " valid readv count rejected").c_str());
}

template <typename Iovecs>
bool writevAcceptsCount(galay::async::AsyncTcpSocket& socket,
                        Iovecs& iovecs,
                        size_t count,
                        const std::string& label)
{
    auto awaitable = socket.writev(iovecs, count);
    return check(!awaitable.await_ready(), (label + " valid writev count rejected").c_str());
}

template <typename Iovecs>
bool readvRejectsCount(galay::async::AsyncTcpSocket& socket,
                       Iovecs& iovecs,
                       size_t count,
                       const std::string& label)
{
    auto awaitable = socket.readv(iovecs, count);
    if (!check(awaitable.await_ready(), (label + " invalid readv count did not complete immediately").c_str())) {
        return false;
    }
    return check(hasParamInvalid(awaitable.await_resume()),
                 (label + " invalid readv count did not return kParamInvalid").c_str());
}

template <typename Iovecs>
bool writevRejectsCount(galay::async::AsyncTcpSocket& socket,
                        Iovecs& iovecs,
                        size_t count,
                        const std::string& label)
{
    auto awaitable = socket.writev(iovecs, count);
    if (!check(awaitable.await_ready(), (label + " invalid writev count did not complete immediately").c_str())) {
        return false;
    }
    return check(hasParamInvalid(awaitable.await_resume()),
                 (label + " invalid writev count did not return kParamInvalid").c_str());
}

template <size_t N>
bool testStdArrayBounds()
{
    galay::async::AsyncTcpSocket socket(GHandle::invalid());
    std::array<struct iovec, N> iovecs{};
    const std::string prefix = "std::array<" + std::to_string(N) + ">";

    bool ok = true;
    ok = readvAcceptsCount(socket, iovecs, 0, prefix) && ok;
    ok = readvAcceptsCount(socket, iovecs, N, prefix) && ok;
    ok = writevAcceptsCount(socket, iovecs, 0, prefix) && ok;
    ok = writevAcceptsCount(socket, iovecs, N, prefix) && ok;
    ok = readvRejectsCount(socket, iovecs, N + 1, prefix) && ok;
    ok = writevRejectsCount(socket, iovecs, N + 1, prefix) && ok;
    ok = readvRejectsCount(socket, iovecs, std::numeric_limits<size_t>::max(), prefix) && ok;
    ok = writevRejectsCount(socket, iovecs, std::numeric_limits<size_t>::max(), prefix) && ok;
    return ok;
}

template <size_t N>
bool testCArrayBounds()
{
    galay::async::AsyncTcpSocket socket(GHandle::invalid());
    struct iovec iovecs[N]{};
    const std::string prefix = "C array[" + std::to_string(N) + "]";

    bool ok = true;
    ok = readvAcceptsCount(socket, iovecs, 0, prefix) && ok;
    ok = readvAcceptsCount(socket, iovecs, N, prefix) && ok;
    ok = writevAcceptsCount(socket, iovecs, 0, prefix) && ok;
    ok = writevAcceptsCount(socket, iovecs, N, prefix) && ok;
    ok = readvRejectsCount(socket, iovecs, N + 1, prefix) && ok;
    ok = writevRejectsCount(socket, iovecs, N + 1, prefix) && ok;
    ok = readvRejectsCount(socket, iovecs, std::numeric_limits<size_t>::max(), prefix) && ok;
    ok = writevRejectsCount(socket, iovecs, std::numeric_limits<size_t>::max(), prefix) && ok;
    return ok;
}

}  // namespace

int main()
{
    bool ok = true;
    ok = testStdArrayBounds<1>() && ok;
    ok = testStdArrayBounds<2>() && ok;
    ok = testCArrayBounds<1>() && ok;
    ok = testCArrayBounds<2>() && ok;
    return ok ? 0 : 1;
}
