/**
 * @file t134_host_validation.cc
 * @brief 验证 Host 字符串构造和 socket bind 的参数错误边界。
 */

#include <galay/cpp/galay-kernel/async/tcp_socket.h>
#include <galay/cpp/galay-kernel/async/udp_socket.h>

#include <iostream>

namespace {

bool check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "[t134] " << message << '\n';
    }
    return condition;
}

bool hasParamInvalid(const std::expected<void, galay::kernel::IOError>& result)
{
    return !result && galay::kernel::IOError::contains(result.error().code(), galay::kernel::kParamInvalid);
}

bool invalidHostStringIsRejected()
{
    galay::kernel::Host host(galay::kernel::IPType::IPV4, "not-an-ip", 0);

    bool ok = check(!host.valid(), "invalid IPv4 string should produce invalid Host");
    ok = check(!host.isIPv4(), "invalid IPv4 string should not report IPv4") && ok;
    ok = check(host.ip().empty(), "invalid Host should expose empty ip string") && ok;
    ok = check(host.port() == 0, "invalid Host should expose port 0") && ok;
    return ok;
}

bool invalidHostFamilyIsRejected()
{
    auto invalidType = static_cast<galay::kernel::IPType>(99);
    galay::kernel::Host host(invalidType, "127.0.0.1", 0);

    return check(!host.valid(), "invalid IPType should produce invalid Host");
}

bool tcpBindRejectsInvalidHost()
{
    galay::async::TcpSocket socket(GHandle::invalid());
    galay::kernel::Host host(galay::kernel::IPType::IPV4, "not-an-ip", 0);

    return check(hasParamInvalid(socket.bind(host)), "tcp bind should return kParamInvalid for invalid Host");
}

bool udpBindRejectsInvalidHost()
{
    galay::async::UdpSocket socket(GHandle::invalid());
    galay::kernel::Host host(galay::kernel::IPType::IPV4, "not-an-ip", 0);

    return check(hasParamInvalid(socket.bind(host)), "udp bind should return kParamInvalid for invalid Host");
}

} // namespace

int main()
{
    bool ok = true;
    ok = invalidHostStringIsRejected() && ok;
    ok = invalidHostFamilyIsRejected() && ok;
    ok = tcpBindRejectsInvalidHost() && ok;
    ok = udpBindRejectsInvalidHost() && ok;
    return ok ? 0 : 1;
}
