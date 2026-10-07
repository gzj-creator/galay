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

bool has_param_invalid(const std::expected<size_t, galay::kernel::IOError>& result)
{
    return !result && galay::kernel::IOError::contains(result.error().code(), galay::kernel::kParamInvalid);
}

template <typename Iovecs>
bool readv_accepts_count(galay::async::AsyncTcpSocket& socket,
                       Iovecs& iovecs,
                       size_t count,
                       const std::string& label)
{
    auto awaitable = socket.readv(iovecs, count);
    return check(!awaitable.await_ready(), (label + " valid readv count rejected").c_str());
}

template <typename Iovecs>
bool writev_accepts_count(galay::async::AsyncTcpSocket& socket,
                        Iovecs& iovecs,
                        size_t count,
                        const std::string& label)
{
    auto awaitable = socket.writev(iovecs, count);
    return check(!awaitable.await_ready(), (label + " valid writev count rejected").c_str());
}

template <typename Iovecs>
bool readv_rejects_count(galay::async::AsyncTcpSocket& socket,
                       Iovecs& iovecs,
                       size_t count,
                       const std::string& label)
{
    auto awaitable = socket.readv(iovecs, count);
    if (!check(awaitable.await_ready(), (label + " invalid readv count did not complete immediately").c_str())) {
        return false;
    }
    return check(has_param_invalid(awaitable.await_resume()),
                 (label + " invalid readv count did not return kParamInvalid").c_str());
}

template <typename Iovecs>
bool writev_rejects_count(galay::async::AsyncTcpSocket& socket,
                        Iovecs& iovecs,
                        size_t count,
                        const std::string& label)
{
    auto awaitable = socket.writev(iovecs, count);
    if (!check(awaitable.await_ready(), (label + " invalid writev count did not complete immediately").c_str())) {
        return false;
    }
    return check(has_param_invalid(awaitable.await_resume()),
                 (label + " invalid writev count did not return kParamInvalid").c_str());
}

template <size_t N>
bool test_std_array_bounds()
{
    galay::async::AsyncTcpSocket socket(GHandle::invalid());
    std::array<struct iovec, N> iovecs{};
    const std::string prefix = "std::array<" + std::to_string(N) + ">";

    bool ok = true;
    ok = readv_accepts_count(socket, iovecs, 0, prefix) && ok;
    ok = readv_accepts_count(socket, iovecs, N, prefix) && ok;
    ok = writev_accepts_count(socket, iovecs, 0, prefix) && ok;
    ok = writev_accepts_count(socket, iovecs, N, prefix) && ok;
    ok = readv_rejects_count(socket, iovecs, N + 1, prefix) && ok;
    ok = writev_rejects_count(socket, iovecs, N + 1, prefix) && ok;
    ok = readv_rejects_count(socket, iovecs, std::numeric_limits<size_t>::max(), prefix) && ok;
    ok = writev_rejects_count(socket, iovecs, std::numeric_limits<size_t>::max(), prefix) && ok;
    return ok;
}

template <size_t N>
bool test_c_array_bounds()
{
    galay::async::AsyncTcpSocket socket(GHandle::invalid());
    struct iovec iovecs[N]{};
    const std::string prefix = "C array[" + std::to_string(N) + "]";

    bool ok = true;
    ok = readv_accepts_count(socket, iovecs, 0, prefix) && ok;
    ok = readv_accepts_count(socket, iovecs, N, prefix) && ok;
    ok = writev_accepts_count(socket, iovecs, 0, prefix) && ok;
    ok = writev_accepts_count(socket, iovecs, N, prefix) && ok;
    ok = readv_rejects_count(socket, iovecs, N + 1, prefix) && ok;
    ok = writev_rejects_count(socket, iovecs, N + 1, prefix) && ok;
    ok = readv_rejects_count(socket, iovecs, std::numeric_limits<size_t>::max(), prefix) && ok;
    ok = writev_rejects_count(socket, iovecs, std::numeric_limits<size_t>::max(), prefix) && ok;
    return ok;
}

}  // namespace

int main()
{
    bool ok = true;
    ok = test_std_array_bounds<1>() && ok;
    ok = test_std_array_bounds<2>() && ok;
    ok = test_c_array_bounds<1>() && ok;
    ok = test_c_array_bounds<2>() && ok;
    return ok ? 0 : 1;
}
