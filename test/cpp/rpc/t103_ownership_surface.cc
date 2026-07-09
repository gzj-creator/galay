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

bool viewEquals(RpcPayloadView view, std::string_view expected)
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

bool requestOwnedCloneDeepCopies()
{
    std::string payload = "request-owned-payload";
    RpcRequest request(7, "OwnerService", "echo");
    request.callMode(RpcCallMode::CLIENT_STREAMING);
    request.endOfStream(false);
    request.payload(payload.data(), payload.size());
    if (!request.metadata().insert("traceparent", "00-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa-bbbbbbbbbbbbbbbb-01").has_value()) {
        return false;
    }

    RpcPayloadView before = request.payloadView();
    RpcRequest moved(std::move(request));
    RpcPayloadView moved_view = moved.payloadView();
    if (moved_view.segment1 != moved.payload().data() ||
        !viewEquals(moved_view, payload) ||
        moved.requestId() != 7 ||
        moved.callMode() != RpcCallMode::CLIENT_STREAMING ||
        moved.endOfStream()) {
        return false;
    }

    RpcRequest cloned = moved.clone();
    RpcPayloadView cloned_view = cloned.payloadView();
    return cloned_view.segment1 != before.segment1 &&
           cloned_view.segment1 != moved_view.segment1 &&
           cloned_view.segment1 == cloned.payload().data() &&
           viewEquals(cloned_view, payload) &&
           cloned.requestId() == moved.requestId() &&
           cloned.serviceName() == moved.serviceName() &&
           cloned.methodName() == moved.methodName() &&
           cloned.callMode() == moved.callMode() &&
           cloned.endOfStream() == moved.endOfStream() &&
           cloned.metadata().get("traceparent").has_value();
}

bool requestBorrowedCloneDeepCopies()
{
    const std::string first = "request-";
    const std::string second = "borrowed";
    RpcRequest request(8, "BorrowService", "echo");
    request.payloadView(RpcPayloadView{first.data(), first.size(), second.data(), second.size()});

    RpcRequest cloned = request.clone();
    RpcPayloadView source_view = request.payloadView();
    RpcPayloadView cloned_view = cloned.payloadView();
    return source_view.segment1 == first.data() &&
           source_view.segment2 == second.data() &&
           cloned_view.segment1 != first.data() &&
           cloned_view.segment1 != second.data() &&
           cloned_view.segment2 == nullptr &&
           cloned_view.segment1 == cloned.payload().data() &&
           viewEquals(cloned_view, first + second);
}

bool responseOwnedCloneDeepCopies()
{
    std::string payload = "response-owned-payload";
    RpcResponse response(9, RpcErrorCode::OK);
    response.callMode(RpcCallMode::SERVER_STREAMING);
    response.endOfStream(false);
    response.payload(payload.data(), payload.size());

    RpcPayloadView before = response.payloadView();
    RpcResponse moved(std::move(response));
    RpcPayloadView moved_view = moved.payloadView();
    if (moved_view.segment1 != moved.payload().data() ||
        !viewEquals(moved_view, payload) ||
        moved.requestId() != 9 ||
        moved.callMode() != RpcCallMode::SERVER_STREAMING ||
        moved.endOfStream()) {
        return false;
    }

    RpcResponse cloned = moved.clone();
    RpcPayloadView cloned_view = cloned.payloadView();
    return cloned_view.segment1 != before.segment1 &&
           cloned_view.segment1 != moved_view.segment1 &&
           cloned_view.segment1 == cloned.payload().data() &&
           viewEquals(cloned_view, payload) &&
           cloned.requestId() == moved.requestId() &&
           cloned.errorCode() == moved.errorCode() &&
           cloned.callMode() == moved.callMode() &&
           cloned.endOfStream() == moved.endOfStream();
}

bool responseBorrowedCloneDeepCopies()
{
    const std::string first = "response-";
    const std::string second = "borrowed";
    RpcResponse response(10, RpcErrorCode::OK);
    response.payloadView(RpcPayloadView{first.data(), first.size(), second.data(), second.size()});

    RpcResponse cloned = response.clone();
    RpcPayloadView source_view = response.payloadView();
    RpcPayloadView cloned_view = cloned.payloadView();
    return source_view.segment1 == first.data() &&
           source_view.segment2 == second.data() &&
           cloned_view.segment1 != first.data() &&
           cloned_view.segment1 != second.data() &&
           cloned_view.segment2 == nullptr &&
           cloned_view.segment1 == cloned.payload().data() &&
           viewEquals(cloned_view, first + second);
}

bool streamMessageOwnedCloneDeepCopies()
{
    std::string payload = "stream-owned-payload";
    StreamMessage message(11, payload.data(), payload.size());
    message.setEnd(true);
    message.messageType(RpcMessageType::STREAM_DATA);

    RpcPayloadView before = message.payloadView();
    StreamMessage moved(std::move(message));
    RpcPayloadView moved_view = moved.payloadView();
    if (moved_view.segment1 != moved.payload().data() ||
        !viewEquals(moved_view, payload) ||
        moved.streamId() != 11 ||
        !moved.isEnd() ||
        moved.messageType() != RpcMessageType::STREAM_DATA) {
        return false;
    }

    StreamMessage cloned = moved.clone();
    RpcPayloadView cloned_view = cloned.payloadView();
    return cloned_view.segment1 != before.segment1 &&
           cloned_view.segment1 != moved_view.segment1 &&
           cloned_view.segment1 == cloned.payload().data() &&
           viewEquals(cloned_view, payload) &&
           cloned.streamId() == moved.streamId() &&
           cloned.isEnd() == moved.isEnd() &&
           cloned.messageType() == moved.messageType();
}

bool streamMessageBorrowedCloneDeepCopies()
{
    const std::string first = "stream-";
    const std::string second = "borrowed";
    StreamMessage message;
    message.streamId(12);
    message.payloadView(RpcPayloadView{first.data(), first.size(), second.data(), second.size()});

    StreamMessage cloned = message.clone();
    RpcPayloadView source_view = message.payloadView();
    RpcPayloadView cloned_view = cloned.payloadView();
    return source_view.segment1 == first.data() &&
           source_view.segment2 == second.data() &&
           cloned_view.segment1 != first.data() &&
           cloned_view.segment1 != second.data() &&
           cloned_view.segment2 == nullptr &&
           cloned_view.segment1 == cloned.payload().data() &&
           viewEquals(cloned_view, first + second);
}

} // namespace

int main()
{
    test::TestResultWriter writer("t103_ownership_surface.result");

    std::cout << "Running RPC Ownership Surface Tests...\n";

    writer.writeTestCase("RpcRequest owned clone deep copies payload", requestOwnedCloneDeepCopies());
    writer.writeTestCase("RpcRequest borrowed clone deep copies payload", requestBorrowedCloneDeepCopies());
    writer.writeTestCase("RpcResponse owned clone deep copies payload", responseOwnedCloneDeepCopies());
    writer.writeTestCase("RpcResponse borrowed clone deep copies payload", responseBorrowedCloneDeepCopies());
    writer.writeTestCase("StreamMessage owned clone deep copies payload", streamMessageOwnedCloneDeepCopies());
    writer.writeTestCase("StreamMessage borrowed clone deep copies payload", streamMessageBorrowedCloneDeepCopies());

    writer.writeSummary();

    std::cout << "Tests completed. Passed: " << writer.passed()
              << ", Failed: " << writer.failed() << "\n";

    return writer.failed() > 0 ? 1 : 0;
}
