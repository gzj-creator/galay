/**
 * @file t103_ownership_surface.cc
 * @brief RPC ownership类型表面测试
 */

#include "result_writer.h"
#include <galay/cpp/galay-rpc/kernel/rpc_config.h>
#include <galay/cpp/galay-rpc/kernel/rpc_conn.h>
#include <galay/cpp/galay-rpc/kernel/rpc_endpoint.h>
#include <galay/cpp/galay-rpc/kernel/rpc_service.h>
#include <galay/cpp/galay-rpc/kernel/rpc_stream.h>
#include <galay/cpp/galay-rpc/protoc/rpc_message.h>

#include <cstring>
#include <iostream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace galay::rpc;

namespace {

template<typename T>
constexpr bool has_noexcept_public_move_v =
    std::is_move_constructible_v<T> &&
    std::is_move_assignable_v<T> &&
    std::is_nothrow_move_constructible_v<T> &&
    std::is_nothrow_move_assignable_v<T>;

template<typename T>
constexpr bool has_noexcept_public_move_ctor_v =
    std::is_move_constructible_v<T> &&
    std::is_nothrow_move_constructible_v<T>;

template<typename T>
constexpr bool is_move_only_owner_v =
    !std::is_copy_constructible_v<T> &&
    !std::is_copy_assignable_v<T> &&
    has_noexcept_public_move_v<T>;

static_assert(!std::is_copy_constructible_v<RpcService>);
static_assert(!std::is_copy_assignable_v<RpcService>);
static_assert(!std::is_move_constructible_v<RpcService>);
static_assert(!std::is_move_assignable_v<RpcService>);

static_assert(is_move_only_owner_v<RpcRequest>);
static_assert(is_move_only_owner_v<RpcResponse>);
static_assert(is_move_only_owner_v<StreamMessage>);
static_assert(std::is_same_v<decltype(std::declval<const RpcRequest&>().clone()), RpcRequest>);
static_assert(std::is_same_v<decltype(std::declval<const RpcResponse&>().clone()), RpcResponse>);
static_assert(std::is_same_v<decltype(std::declval<const StreamMessage&>().clone()), StreamMessage>);

static_assert(!std::is_copy_constructible_v<StreamReader>);
static_assert(!std::is_copy_assignable_v<StreamReader>);
static_assert(has_noexcept_public_move_ctor_v<StreamReader>);
static_assert(!std::is_copy_constructible_v<StreamWriter>);
static_assert(!std::is_copy_assignable_v<StreamWriter>);
static_assert(has_noexcept_public_move_ctor_v<StreamWriter>);
static_assert(!std::is_copy_constructible_v<RpcStream>);
static_assert(!std::is_copy_assignable_v<RpcStream>);
static_assert(has_noexcept_public_move_ctor_v<RpcStream>);
static_assert(!std::is_copy_constructible_v<RpcReader>);
static_assert(!std::is_copy_assignable_v<RpcReader>);
static_assert(has_noexcept_public_move_ctor_v<RpcReader>);
static_assert(!std::is_copy_constructible_v<RpcWriter>);
static_assert(!std::is_copy_assignable_v<RpcWriter>);
static_assert(has_noexcept_public_move_ctor_v<RpcWriter>);

static_assert(!std::is_copy_constructible_v<galay::rpc::detail::RpcWriteStateBase<>>);
static_assert(!std::is_copy_assignable_v<galay::rpc::detail::RpcWriteStateBase<>>);
static_assert(!std::is_move_constructible_v<galay::rpc::detail::RpcWriteStateBase<>>);
static_assert(!std::is_move_assignable_v<galay::rpc::detail::RpcWriteStateBase<>>);
static_assert(!std::is_copy_constructible_v<galay::rpc::detail::RpcVectorWriteState>);
static_assert(!std::is_copy_assignable_v<galay::rpc::detail::RpcVectorWriteState>);
static_assert(!std::is_move_constructible_v<galay::rpc::detail::RpcVectorWriteState>);
static_assert(!std::is_move_assignable_v<galay::rpc::detail::RpcVectorWriteState>);
static_assert(!std::is_copy_constructible_v<galay::rpc::detail::RpcRequestWriteState>);
static_assert(!std::is_copy_assignable_v<galay::rpc::detail::RpcRequestWriteState>);
static_assert(!std::is_move_constructible_v<galay::rpc::detail::RpcRequestWriteState>);
static_assert(!std::is_move_assignable_v<galay::rpc::detail::RpcRequestWriteState>);
static_assert(!std::is_copy_constructible_v<galay::rpc::detail::RpcResponseWriteState>);
static_assert(!std::is_copy_assignable_v<galay::rpc::detail::RpcResponseWriteState>);
static_assert(!std::is_move_constructible_v<galay::rpc::detail::RpcResponseWriteState>);
static_assert(!std::is_move_assignable_v<galay::rpc::detail::RpcResponseWriteState>);
static_assert(!std::is_copy_constructible_v<galay::rpc::detail::StreamFrameWriteState>);
static_assert(!std::is_copy_assignable_v<galay::rpc::detail::StreamFrameWriteState>);
static_assert(!std::is_move_constructible_v<galay::rpc::detail::StreamFrameWriteState>);
static_assert(!std::is_move_assignable_v<galay::rpc::detail::StreamFrameWriteState>);

static_assert(std::is_copy_constructible_v<RpcHeader>);
static_assert(std::is_copy_assignable_v<RpcHeader>);
static_assert(std::is_copy_constructible_v<RpcPayloadView>);
static_assert(std::is_copy_assignable_v<RpcPayloadView>);
static_assert(std::is_copy_constructible_v<RpcMetadata>);
static_assert(std::is_copy_assignable_v<RpcMetadata>);
static_assert(std::is_copy_constructible_v<RpcCallOptions>);
static_assert(std::is_copy_assignable_v<RpcCallOptions>);
static_assert(std::is_copy_constructible_v<RpcServerRuntimeConfig>);
static_assert(std::is_copy_assignable_v<RpcServerRuntimeConfig>);
static_assert(std::is_copy_constructible_v<RpcClientRuntimeConfig>);
static_assert(std::is_copy_assignable_v<RpcClientRuntimeConfig>);
static_assert(std::is_copy_constructible_v<RpcDiscoveryConfig>);
static_assert(std::is_copy_assignable_v<RpcDiscoveryConfig>);
static_assert(std::is_copy_constructible_v<RpcBenchmarkConfig>);
static_assert(std::is_copy_assignable_v<RpcBenchmarkConfig>);
static_assert(std::is_copy_constructible_v<RpcConfig>);
static_assert(std::is_copy_assignable_v<RpcConfig>);
static_assert(std::is_copy_constructible_v<RpcEndpoint>);
static_assert(std::is_copy_assignable_v<RpcEndpoint>);
static_assert(std::is_copy_constructible_v<RpcEndpointInfo>);
static_assert(std::is_copy_assignable_v<RpcEndpointInfo>);

bool view_equals(RpcPayloadView view, std::string_view expected)
{
    if (view.size() != expected.size()) {
        return false;
    }
    if (view.segment1_len > 0 &&
        std::memcmp(view.segment1, expected.data(), view.segment1_len) != 0) {
        return false;
    }
    if (view.segment2_len > 0 &&
        std::memcmp(view.segment2,
                    expected.data() + view.segment1_len,
                    view.segment2_len) != 0) {
        return false;
    }
    return true;
}

bool request_owned_clone_deep_copies()
{
    std::string payload = "request-owned-payload";
    RpcRequest request(7, "OwnerService", "echo");
    request.call_mode(RpcCallMode::CLIENT_STREAMING);
    request.end_of_stream(false);
    request.payload(payload.data(), payload.size());
    if (!request.metadata().insert("traceparent", "00-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-bbbbbbbbbbbbbbbb-01").has_value()) {
        return false;
    }

    RpcPayloadView before = request.payload_view();
    RpcRequest moved(std::move(request));
    RpcPayloadView moved_view = moved.payload_view();
    if (moved_view.segment1 != moved.payload().data() ||
        !view_equals(moved_view, payload) ||
        moved.request_id() != 7 ||
        moved.call_mode() != RpcCallMode::CLIENT_STREAMING ||
        moved.end_of_stream()) {
        return false;
    }

    RpcRequest cloned = moved.clone();
    RpcPayloadView cloned_view = cloned.payload_view();
    return cloned_view.segment1 != before.segment1 &&
           cloned_view.segment1 != moved_view.segment1 &&
           cloned_view.segment1 == cloned.payload().data() &&
           view_equals(cloned_view, payload) &&
           cloned.request_id() == moved.request_id() &&
           cloned.service_name() == moved.service_name() &&
           cloned.method_name() == moved.method_name() &&
           cloned.call_mode() == moved.call_mode() &&
           cloned.end_of_stream() == moved.end_of_stream() &&
           cloned.metadata().get("traceparent").has_value();
}

bool request_borrowed_clone_deep_copies()
{
    const std::string first = "request-";
    const std::string second = "borrowed";
    RpcRequest request(8, "BorrowService", "echo");
    request.payload_view(RpcPayloadView{first.data(), first.size(), second.data(), second.size()});

    RpcRequest cloned = request.clone();
    RpcPayloadView source_view = request.payload_view();
    RpcPayloadView cloned_view = cloned.payload_view();
    return source_view.segment1 == first.data() &&
           source_view.segment2 == second.data() &&
           cloned_view.segment1 != first.data() &&
           cloned_view.segment1 != second.data() &&
           cloned_view.segment2 == nullptr &&
           cloned_view.segment1 == cloned.payload().data() &&
           view_equals(cloned_view, first + second);
}

bool response_owned_clone_deep_copies()
{
    std::string payload = "response-owned-payload";
    RpcResponse response(9, RpcErrorCode::OK);
    response.call_mode(RpcCallMode::SERVER_STREAMING);
    response.end_of_stream(false);
    response.payload(payload.data(), payload.size());

    RpcPayloadView before = response.payload_view();
    RpcResponse moved(std::move(response));
    RpcPayloadView moved_view = moved.payload_view();
    if (moved_view.segment1 != moved.payload().data() ||
        !view_equals(moved_view, payload) ||
        moved.request_id() != 9 ||
        moved.call_mode() != RpcCallMode::SERVER_STREAMING ||
        moved.end_of_stream()) {
        return false;
    }

    RpcResponse cloned = moved.clone();
    RpcPayloadView cloned_view = cloned.payload_view();
    return cloned_view.segment1 != before.segment1 &&
           cloned_view.segment1 != moved_view.segment1 &&
           cloned_view.segment1 == cloned.payload().data() &&
           view_equals(cloned_view, payload) &&
           cloned.request_id() == moved.request_id() &&
           cloned.error_code() == moved.error_code() &&
           cloned.call_mode() == moved.call_mode() &&
           cloned.end_of_stream() == moved.end_of_stream();
}

bool response_borrowed_clone_deep_copies()
{
    const std::string first = "response-";
    const std::string second = "borrowed";
    RpcResponse response(10, RpcErrorCode::OK);
    response.payload_view(RpcPayloadView{first.data(), first.size(), second.data(), second.size()});

    RpcResponse cloned = response.clone();
    RpcPayloadView source_view = response.payload_view();
    RpcPayloadView cloned_view = cloned.payload_view();
    return source_view.segment1 == first.data() &&
           source_view.segment2 == second.data() &&
           cloned_view.segment1 != first.data() &&
           cloned_view.segment1 != second.data() &&
           cloned_view.segment2 == nullptr &&
           cloned_view.segment1 == cloned.payload().data() &&
           view_equals(cloned_view, first + second);
}

bool stream_message_owned_clone_deep_copies()
{
    std::string payload = "stream-owned-payload";
    StreamMessage message(11, payload.data(), payload.size());
    message.set_end(true);
    message.message_type(RpcMessageType::STREAM_DATA);

    RpcPayloadView before = message.payload_view();
    StreamMessage moved(std::move(message));
    RpcPayloadView moved_view = moved.payload_view();
    if (moved_view.segment1 != moved.payload().data() ||
        !view_equals(moved_view, payload) ||
        moved.stream_id() != 11 ||
        !moved.is_end() ||
        moved.message_type() != RpcMessageType::STREAM_DATA) {
        return false;
    }

    StreamMessage cloned = moved.clone();
    RpcPayloadView cloned_view = cloned.payload_view();
    return cloned_view.segment1 != before.segment1 &&
           cloned_view.segment1 != moved_view.segment1 &&
           cloned_view.segment1 == cloned.payload().data() &&
           view_equals(cloned_view, payload) &&
           cloned.stream_id() == moved.stream_id() &&
           cloned.is_end() == moved.is_end() &&
           cloned.message_type() == moved.message_type();
}

bool stream_message_borrowed_clone_deep_copies()
{
    const std::string first = "stream-";
    const std::string second = "borrowed";
    StreamMessage message;
    message.stream_id(12);
    message.payload_view(RpcPayloadView{first.data(), first.size(), second.data(), second.size()});

    StreamMessage cloned = message.clone();
    RpcPayloadView source_view = message.payload_view();
    RpcPayloadView cloned_view = cloned.payload_view();
    return source_view.segment1 == first.data() &&
           source_view.segment2 == second.data() &&
           cloned_view.segment1 != first.data() &&
           cloned_view.segment1 != second.data() &&
           cloned_view.segment2 == nullptr &&
           cloned_view.segment1 == cloned.payload().data() &&
           view_equals(cloned_view, first + second);
}

} // namespace

int main()
{
    test::TestResultWriter writer("t103_ownership_surface.result");

    std::cout << "Running RPC Ownership Surface Tests...\n";

    writer.write_test_case("RpcRequest owned clone deep copies payload", request_owned_clone_deep_copies());
    writer.write_test_case("RpcRequest borrowed clone deep copies payload", request_borrowed_clone_deep_copies());
    writer.write_test_case("RpcResponse owned clone deep copies payload", response_owned_clone_deep_copies());
    writer.write_test_case("RpcResponse borrowed clone deep copies payload", response_borrowed_clone_deep_copies());
    writer.write_test_case("StreamMessage owned clone deep copies payload", stream_message_owned_clone_deep_copies());
    writer.write_test_case("StreamMessage borrowed clone deep copies payload", stream_message_borrowed_clone_deep_copies());

    writer.write_summary();

    std::cout << "Tests completed. Passed: " << writer.passed()
              << ", Failed: " << writer.failed() << "\n";

    return writer.failed() > 0 ? 1 : 0;
}
