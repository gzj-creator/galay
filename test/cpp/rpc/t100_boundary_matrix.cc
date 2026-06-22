#include <galay/cpp/galay-rpc/kernel/rpc_call.h>
#include <galay/cpp/galay-rpc/kernel/rpc_channel.h>
#include <galay/cpp/galay-rpc/kernel/rpc_client.h>
#include <galay/cpp/galay-rpc/kernel/rpc_managed_client.h>
#include <galay/cpp/galay-rpc/protoc/rpc_codec.h>
#include <galay/cpp/galay-rpc/protoc/rpc_message.h>

#include <cstring>
#include <iostream>
#include <string>

using namespace galay::rpc;

namespace {

int expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << "\n";
        return 1;
    }
    return 0;
}

} // namespace

int main()
{
    RpcRequest empty_route(1, "", "");
    auto empty_wire = empty_route.serialize();
    auto empty_decoded = RpcCodec::decodeRequest(empty_wire.data(), empty_wire.size());
    if (auto rc = expect(empty_decoded.has_value() &&
                             empty_decoded->serviceName().empty() &&
                             empty_decoded->methodName().empty(),
                         "empty service/method did not round trip")) {
        return rc;
    }

    std::string max_name(255, 'a');
    RpcRequest max_route(2, max_name, max_name);
    auto max_wire = max_route.serialize();
    auto max_decoded = RpcCodec::decodeRequest(max_wire.data(), max_wire.size());
    if (auto rc = expect(max_decoded.has_value() &&
                             max_decoded->serviceName() == max_name &&
                             max_decoded->methodName() == max_name,
                         "max service/method length did not round trip")) {
        return rc;
    }

    char header_buf[RPC_HEADER_SIZE]{};
    RpcHeader wrong_magic;
    wrong_magic.m_magic = 0;
    wrong_magic.m_type = static_cast<uint8_t>(RpcMessageType::REQUEST);
    wrong_magic.serialize(header_buf);
    if (auto rc = expect(!RpcCodec::decodeRequest(header_buf, sizeof(header_buf)).has_value(),
                         "wrong magic header was accepted")) {
        return rc;
    }

    RpcHeader oversized;
    oversized.m_type = static_cast<uint8_t>(RpcMessageType::REQUEST);
    oversized.m_body_length = RPC_MAX_BODY_SIZE + 1;
    oversized.serialize(header_buf);
    if (auto rc = expect(!RpcCodec::decodeRequest(header_buf, sizeof(header_buf)).has_value(),
                         "oversized body header was accepted")) {
        return rc;
    }

    RpcClient stream_client;
    auto stream_before_connect = stream_client.createStream(1, "StreamService", "open");
    if (auto rc = expect(!stream_before_connect.has_value() &&
                             stream_before_connect.error().code() == RpcErrorCode::CONNECTION_CLOSED,
                         "createStream before connect did not return connection closed")) {
        return rc;
    }

    RpcCallOptions lifetime_options;
    auto client_task = stream_client.call(std::string("LifetimeService"),
                                          std::string("Echo"),
                                          std::string("payload"),
                                          lifetime_options);
    (void)client_task;
    const char borrowed_payload[] = "borrowed";
    auto client_buffer_task = stream_client.call(std::string("LifetimeService"),
                                                 std::string("Echo"),
                                                 borrowed_payload,
                                                 sizeof(borrowed_payload) - 1,
                                                 RpcCallOptions{});
    (void)client_buffer_task;

    RpcStaticDiscovery lifetime_discovery;
    RpcManagedClient managed_client(lifetime_discovery);
    auto managed_task = managed_client.call(std::string("LifetimeService"),
                                            std::string("Echo"),
                                            std::string("payload"),
                                            RpcCallOptions{});
    (void)managed_task;

    auto truncated = max_wire;
    truncated.resize(truncated.size() - 1);
    if (auto rc = expect(!RpcCodec::decodeRequest(truncated.data(), truncated.size()).has_value(),
                         "truncated request body was accepted")) {
        return rc;
    }

    RpcChannelOptions options;
    options.max_in_flight = 1;
    options.max_outbound_queue = 1;
    RpcChannelState state(options);
    auto first = state.registerPending(9);
    auto duplicate = state.registerPending(9);
    if (auto rc = expect(first.has_value() && !duplicate.has_value() &&
                             duplicate.error().code() == RpcErrorCode::RESOURCE_EXHAUSTED,
                         "duplicate/pressure pending boundary did not reject")) {
        return rc;
    }
    state.failPending(9, RpcError(RpcErrorCode::CANCELLED, "cleanup"));

    RpcOutboundBackpressure outbound(options);
    auto reserve = outbound.reserve(1);
    auto queued = outbound.reserve(1);
    if (auto rc = expect(reserve.has_value() && !queued.has_value() &&
                             queued.error().code() == RpcErrorCode::RESOURCE_EXHAUSTED,
                         "outbound queue boundary did not reject")) {
        return rc;
    }
    outbound.release(1);
    if (auto rc = expect(outbound.reserve(1).has_value(),
                         "outbound rejection poisoned subsequent valid reservation")) {
        return rc;
    }

    RpcCallOptions expired;
    expired.deadline(RpcClock::now());
    if (auto rc = expect(expired.effectiveDeadline(RpcClock::now()).has_value(),
                         "deadline while queued was not representable")) {
        return rc;
    }

    RpcCancellationSource source;
    auto token = source.token();
    source.cancel();
    if (auto rc = expect(token.cancelled(), "cancellation while queued was not observable")) {
        return rc;
    }

    std::cout << "RPC boundary matrix PASS\n";
    return 0;
}
