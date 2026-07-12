/**
 * @file t104_server_error_surface.cc
 * @brief RPC服务器注册与启动错误传播表面测试
 */

#include "result_writer.h"

#include <galay/cpp/galay-rpc/kernel/rpc_server.h>
#include <galay/cpp/galay-rpc/kernel/streamsvc.h>

#include <arpa/inet.h>
#include <array>
#include <concepts>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <sys/socket.h>
#include <utility>

using namespace galay::async;
using namespace galay::kernel;
using namespace galay::rpc;

namespace {

class EmptyService final : public RpcService {
public:
    explicit EmptyService(std::string_view name)
        : RpcService(name) {}
};

template<typename Server>
concept SharedPtrServiceRegistration = requires(
    Server& server,
    std::shared_ptr<RpcService> service) {
    server.registerService(std::move(service));
};

static_assert(std::same_as<
    decltype(std::declval<RpcServer&>().registerService(std::declval<RpcService&>())),
    std::expected<void, RpcError>>);
static_assert(std::same_as<
    decltype(std::declval<RpcStreamServer&>().registerService(std::declval<RpcService&>())),
    std::expected<void, RpcError>>);
static_assert(!SharedPtrServiceRegistration<RpcServer>);
static_assert(!SharedPtrServiceRegistration<RpcStreamServer>);
static_assert(std::same_as<
    decltype(std::declval<RpcServer&>().start()),
    std::expected<void, RpcError>>);
static_assert(std::same_as<
    decltype(std::declval<RpcStreamServer&>().start()),
    std::expected<void, RpcError>>);

std::expected<std::pair<TcpSocket, uint16_t>, std::string> reservePort()
{
    auto listener = TcpSocket::create(IPType::IPV4);
    if (!listener.has_value()) {
        return std::unexpected("failed to create blocking listener");
    }

    Host host(IPType::IPV4, "127.0.0.1", 0);
    auto bound = listener->bind(host);
    if (!bound.has_value()) {
        return std::unexpected("failed to bind blocking listener");
    }
    auto listening = listener->listen(1);
    if (!listening.has_value()) {
        return std::unexpected("failed to listen on blocking listener");
    }

    sockaddr_in address{};
    socklen_t address_length = sizeof(address);
    const int rc = ::getsockname(
        listener->handle().fd,
        reinterpret_cast<sockaddr*>(&address),
        &address_length);
    if (rc != 0) {
        return std::unexpected("failed to query blocking listener port");
    }

    return std::pair<TcpSocket, uint16_t>(
        std::move(*listener),
        ntohs(address.sin_port));
}

void testServiceRegistration(test::TestResultWriter& writer)
{
    RpcServer server = RpcServerBuilder().build();
    EmptyService first("duplicate");
    EmptyService second("duplicate");

    auto registered = server.registerService(first);
    auto duplicate = server.registerService(second);
    writer.writeTestCase(
        "RpcServer registration returns explicit duplicate error",
        registered.has_value() &&
            !duplicate.has_value() &&
            duplicate.error().code() == RpcErrorCode::INVALID_REQUEST);

    RpcStreamServer stream_server = RpcStreamServerBuilder().build();
    auto stream_registered = stream_server.registerService(first);
    auto stream_duplicate = stream_server.registerService(second);
    writer.writeTestCase(
        "RpcStreamServer registration returns explicit duplicate error",
        stream_registered.has_value() &&
            !stream_duplicate.has_value() &&
            stream_duplicate.error().code() == RpcErrorCode::INVALID_REQUEST);
}

void testServiceCapacity(test::TestResultWriter& writer)
{
    std::array<std::string, RpcServer::kMaxRegisteredServices + 1> names;
    std::array<std::optional<EmptyService>, RpcServer::kMaxRegisteredServices + 1> services;
    for (size_t i = 0; i < services.size(); ++i) {
        names[i] = "service-" + std::to_string(i);
        services[i].emplace(names[i]);
    }

    RpcServer server = RpcServerBuilder().build();
    bool unary_registered = true;
    for (size_t i = 0; i < RpcServer::kMaxRegisteredServices; ++i) {
        auto result = server.registerService(*services[i]);
        if (!result.has_value()) {
            unary_registered = false;
            break;
        }
    }
    auto unary_overflow = server.registerService(services.back().value());
    writer.writeTestCase(
        "RpcServer registration capacity returns resource exhausted",
        unary_registered &&
            !unary_overflow.has_value() &&
            unary_overflow.error().code() == RpcErrorCode::RESOURCE_EXHAUSTED);

    RpcStreamServer stream_server = RpcStreamServerBuilder().build();
    bool stream_registered = true;
    for (size_t i = 0; i < RpcStreamServer::kMaxRegisteredServices; ++i) {
        auto result = stream_server.registerService(*services[i]);
        if (!result.has_value()) {
            stream_registered = false;
            break;
        }
    }
    auto stream_overflow = stream_server.registerService(services.back().value());
    writer.writeTestCase(
        "RpcStreamServer registration capacity returns resource exhausted",
        stream_registered &&
            !stream_overflow.has_value() &&
            stream_overflow.error().code() == RpcErrorCode::RESOURCE_EXHAUSTED);
}

void testBindFailurePropagation(test::TestResultWriter& writer)
{
    auto reserved = reservePort();
    if (!reserved.has_value()) {
        writer.writeTestCase("reserve occupied port", false);
        return;
    }

    const uint16_t port = reserved->second;
    RpcServer server = RpcServerBuilder()
                           .host("127.0.0.1")
                           .port(port)
                           .ioSchedulerCount(1)
                           .computeSchedulerCount(0)
                           .build();
    auto started = server.start();
    writer.writeTestCase(
        "RpcServer start returns bind failure and remains stopped",
        !started.has_value() && !server.isRunning());

    RpcStreamServer stream_server = RpcStreamServerBuilder()
                                        .host("127.0.0.1")
                                        .port(port)
                                        .ioSchedulerCount(1)
                                        .computeSchedulerCount(0)
                                        .build();
    auto stream_started = stream_server.start();
    writer.writeTestCase(
        "RpcStreamServer start returns bind failure and remains stopped",
        !stream_started.has_value() && !stream_server.isRunning());
}

} // namespace

int main()
{
    test::TestResultWriter writer("t104_server_error_surface.result");
    testServiceRegistration(writer);
    testServiceCapacity(writer);
    testBindFailurePropagation(writer);
    writer.writeSummary();
    return writer.failed() == 0 ? 0 : 1;
}
