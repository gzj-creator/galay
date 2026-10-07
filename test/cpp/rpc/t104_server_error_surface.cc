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
    server.register_service(std::move(service));
};

static_assert(std::same_as<
    decltype(std::declval<RpcServer&>().register_service(std::declval<RpcService&>())),
    std::expected<void, RpcError>>);
static_assert(std::same_as<
    decltype(std::declval<RpcStreamServer&>().register_service(std::declval<RpcService&>())),
    std::expected<void, RpcError>>);
static_assert(!SharedPtrServiceRegistration<RpcServer>);
static_assert(!SharedPtrServiceRegistration<RpcStreamServer>);
static_assert(std::same_as<
    decltype(std::declval<RpcServer&>().start()),
    std::expected<void, RpcError>>);
static_assert(std::same_as<
    decltype(std::declval<RpcStreamServer&>().start()),
    std::expected<void, RpcError>>);

std::expected<std::pair<AsyncTcpSocket, uint16_t>, std::string> reserve_port()
{
    auto listener = AsyncTcpSocket::create(IPType::IPV4);
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

    return std::pair<AsyncTcpSocket, uint16_t>(
        std::move(*listener),
        ntohs(address.sin_port));
}

void test_service_registration(test::TestResultWriter& writer)
{
    RpcServer server = RpcServerBuilder().build();
    EmptyService first("duplicate");
    EmptyService second("duplicate");

    auto registered = server.register_service(first);
    auto duplicate = server.register_service(second);
    writer.write_test_case(
        "RpcServer registration returns explicit duplicate error",
        registered.has_value() &&
            !duplicate.has_value() &&
            duplicate.error().code() == RpcErrorCode::INVALID_REQUEST);

    RpcStreamServer stream_server = RpcStreamServerBuilder().build();
    auto stream_registered = stream_server.register_service(first);
    auto stream_duplicate = stream_server.register_service(second);
    writer.write_test_case(
        "RpcStreamServer registration returns explicit duplicate error",
        stream_registered.has_value() &&
            !stream_duplicate.has_value() &&
            stream_duplicate.error().code() == RpcErrorCode::INVALID_REQUEST);
}

void test_service_capacity(test::TestResultWriter& writer)
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
        auto result = server.register_service(*services[i]);
        if (!result.has_value()) {
            unary_registered = false;
            break;
        }
    }
    auto unary_overflow = server.register_service(services.back().value());
    writer.write_test_case(
        "RpcServer registration capacity returns resource exhausted",
        unary_registered &&
            !unary_overflow.has_value() &&
            unary_overflow.error().code() == RpcErrorCode::RESOURCE_EXHAUSTED);

    RpcStreamServer stream_server = RpcStreamServerBuilder().build();
    bool stream_registered = true;
    for (size_t i = 0; i < RpcStreamServer::kMaxRegisteredServices; ++i) {
        auto result = stream_server.register_service(*services[i]);
        if (!result.has_value()) {
            stream_registered = false;
            break;
        }
    }
    auto stream_overflow = stream_server.register_service(services.back().value());
    writer.write_test_case(
        "RpcStreamServer registration capacity returns resource exhausted",
        stream_registered &&
            !stream_overflow.has_value() &&
            stream_overflow.error().code() == RpcErrorCode::RESOURCE_EXHAUSTED);
}

void test_bind_failure_propagation(test::TestResultWriter& writer)
{
    auto reserved = reserve_port();
    if (!reserved.has_value()) {
        writer.write_test_case("reserve occupied port", false);
        return;
    }

    const uint16_t port = reserved->second;
    RpcServer server = RpcServerBuilder()
                           .host("127.0.0.1")
                           .port(port)
                           .io_scheduler_count(1)
                           .parallel_scheduler_count(0)
                           .build();
    auto started = server.start();
    writer.write_test_case(
        "RpcServer start returns bind failure and remains stopped",
        !started.has_value() && !server.is_running());

    RpcStreamServer stream_server = RpcStreamServerBuilder()
                                        .host("127.0.0.1")
                                        .port(port)
                                        .io_scheduler_count(1)
                                        .parallel_scheduler_count(0)
                                        .build();
    auto stream_started = stream_server.start();
    writer.write_test_case(
        "RpcStreamServer start returns bind failure and remains stopped",
        !stream_started.has_value() && !stream_server.is_running());
}

} // namespace

int main()
{
    test::TestResultWriter writer("t104_server_error_surface.result");
    test_service_registration(writer);
    test_service_capacity(writer);
    test_bind_failure_propagation(writer);
    writer.write_summary();
    return writer.failed() == 0 ? 0 : 1;
}
