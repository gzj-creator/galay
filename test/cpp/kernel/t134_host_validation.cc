/**
 * @file t134_host_validation.cc
 * @brief 验证 Host 字符串构造和 socket bind 的参数错误边界。
 */

#include <galay/cpp/galay-kernel/async/async_tcp.h>
#include <galay/cpp/galay-kernel/async/async_udp.h>

#include <iostream>

namespace {

bool check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "[t134] " << message << '\n';
    }
    return condition;
}

bool has_param_invalid(const std::expected<void, galay::kernel::IOError>& result)
{
    return !result && galay::kernel::IOError::contains(result.error().code(), galay::kernel::kParamInvalid);
}

bool invalid_host_string_is_rejected()
{
    galay::kernel::Host host(galay::kernel::IPType::IPV4, "not-an-ip", 0);

    bool ok = check(!host.valid(), "invalid IPv4 string should produce invalid Host");
    ok = check(!host.is_ipv4(), "invalid IPv4 string should not report IPv4") && ok;
    ok = check(host.ip().empty(), "invalid Host should expose empty ip string") && ok;
    ok = check(host.port() == 0, "invalid Host should expose port 0") && ok;
    return ok;
}

bool invalid_host_family_is_rejected()
{
    auto invalidType = static_cast<galay::kernel::IPType>(99);
    galay::kernel::Host host(invalidType, "127.0.0.1", 0);

    return check(!host.valid(), "invalid IPType should produce invalid Host");
}

bool tcp_bind_rejects_invalid_host()
{
    galay::async::AsyncTcpSocket socket(GHandle::invalid());
    galay::kernel::Host host(galay::kernel::IPType::IPV4, "not-an-ip", 0);

    return check(has_param_invalid(socket.bind(host)), "tcp bind should return kParamInvalid for invalid Host");
}

bool udp_bind_rejects_invalid_host()
{
    galay::async::AsyncUdpSocket socket(GHandle::invalid());
    galay::kernel::Host host(galay::kernel::IPType::IPV4, "not-an-ip", 0);

    return check(has_param_invalid(socket.bind(host)), "udp bind should return kParamInvalid for invalid Host");
}

} // namespace

int main()
{
    bool ok = true;
    ok = invalid_host_string_is_rejected() && ok;
    ok = invalid_host_family_is_rejected() && ok;
    ok = tcp_bind_rejects_invalid_host() && ok;
    ok = udp_bind_rejects_invalid_host() && ok;
    return ok ? 0 : 1;
}
