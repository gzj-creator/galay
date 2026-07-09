#include <galay/cpp/galay-ws/builder/ws_frame_builder.h>
#include <galay/cpp/galay-ws/client/ws_client.h>
#include <galay/cpp/galay-ws/kernel/ws_conn.h>
#include <galay/cpp/galay-ws/protoc/ws_error.h>

#include <concepts>
#include <iostream>
#include <string>
#include <type_traits>
#include <utility>

using galay::async::TcpSocket;
using namespace galay::websocket;
namespace ws_detail = galay::websocket::detail;

namespace {

template <typename T>
concept BuilderClone =
    requires(const T& value) {
        { value.clone() } -> std::same_as<T>;
    };

template <typename T>
constexpr bool kMoveConstructibleOrStableAddress =
    std::is_nothrow_move_constructible_v<T> || !std::is_move_constructible_v<T>;

template <typename T>
constexpr bool kMoveAssignableOrStableAddress =
    std::is_nothrow_move_assignable_v<T> || !std::is_move_assignable_v<T>;

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "[T10] " << message << "\n";
        std::abort();
    }
}

void testBuilderCloneOwnsIndependentFrame()
{
    const std::string payload(4096, 'x');

    WsFrameBuilder source;
    source.text(payload);
    WsFrameBuilder cloned = source.clone();
    WsFrameBuilder moved = std::move(source);

    WsFrame clone_frame = cloned.buildMove();
    WsFrame moved_frame = moved.buildMove();

    require(clone_frame.payload == payload, "builder clone should preserve payload");
    require(moved_frame.payload == payload, "moving builder should preserve source payload");
    require(clone_frame.payload.data() != moved_frame.payload.data(),
            "builder clone should own independent payload storage");
}

} // namespace

static_assert(!std::is_copy_constructible_v<ws_detail::WsFrameReadState>);
static_assert(!std::is_copy_assignable_v<ws_detail::WsFrameReadState>);
static_assert(kMoveConstructibleOrStableAddress<ws_detail::WsFrameReadState>);
static_assert(kMoveAssignableOrStableAddress<ws_detail::WsFrameReadState>);

static_assert(!std::is_copy_constructible_v<ws_detail::WsMessageReadState>);
static_assert(!std::is_copy_assignable_v<ws_detail::WsMessageReadState>);
static_assert(kMoveConstructibleOrStableAddress<ws_detail::WsMessageReadState>);
static_assert(kMoveAssignableOrStableAddress<ws_detail::WsMessageReadState>);

static_assert(!std::is_copy_constructible_v<ws_detail::WsRingBufferTcpReadMachine<ws_detail::WsFrameReadState>>);
static_assert(!std::is_copy_assignable_v<ws_detail::WsRingBufferTcpReadMachine<ws_detail::WsFrameReadState>>);
static_assert(kMoveConstructibleOrStableAddress<ws_detail::WsRingBufferTcpReadMachine<ws_detail::WsFrameReadState>>);
static_assert(kMoveAssignableOrStableAddress<ws_detail::WsRingBufferTcpReadMachine<ws_detail::WsFrameReadState>>);

static_assert(!std::is_copy_constructible_v<WsReaderImpl<TcpSocket>>);
static_assert(!std::is_copy_assignable_v<WsReaderImpl<TcpSocket>>);
static_assert(std::is_nothrow_move_constructible_v<WsReaderImpl<TcpSocket>>);
static_assert(std::is_nothrow_move_assignable_v<WsReaderImpl<TcpSocket>>);

static_assert(!std::is_copy_constructible_v<ws_detail::WsTcpWritevMachine<TcpSocket>>);
static_assert(!std::is_copy_assignable_v<ws_detail::WsTcpWritevMachine<TcpSocket>>);
static_assert(kMoveConstructibleOrStableAddress<ws_detail::WsTcpWritevMachine<TcpSocket>>);
static_assert(kMoveAssignableOrStableAddress<ws_detail::WsTcpWritevMachine<TcpSocket>>);

static_assert(!std::is_copy_constructible_v<WsWriterImpl<TcpSocket>>);
static_assert(!std::is_copy_assignable_v<WsWriterImpl<TcpSocket>>);
static_assert(std::is_nothrow_move_constructible_v<WsWriterImpl<TcpSocket>>);
static_assert(std::is_nothrow_move_assignable_v<WsWriterImpl<TcpSocket>>);

static_assert(!std::is_copy_constructible_v<ws_detail::WsEchoMachine<TcpSocket>>);
static_assert(!std::is_copy_assignable_v<ws_detail::WsEchoMachine<TcpSocket>>);
static_assert(kMoveConstructibleOrStableAddress<ws_detail::WsEchoMachine<TcpSocket>>);
static_assert(kMoveAssignableOrStableAddress<ws_detail::WsEchoMachine<TcpSocket>>);

static_assert(!std::is_copy_constructible_v<ws_detail::WsClientUpgradeState<TcpSocket>>);
static_assert(!std::is_copy_assignable_v<ws_detail::WsClientUpgradeState<TcpSocket>>);
static_assert(kMoveConstructibleOrStableAddress<ws_detail::WsClientUpgradeState<TcpSocket>>);
static_assert(kMoveAssignableOrStableAddress<ws_detail::WsClientUpgradeState<TcpSocket>>);

static_assert(!std::is_copy_constructible_v<ws_detail::WsClientTcpUpgradeMachine<ws_detail::WsClientUpgradeState<TcpSocket>>>);
static_assert(!std::is_copy_assignable_v<ws_detail::WsClientTcpUpgradeMachine<ws_detail::WsClientUpgradeState<TcpSocket>>>);
static_assert(kMoveConstructibleOrStableAddress<ws_detail::WsClientTcpUpgradeMachine<ws_detail::WsClientUpgradeState<TcpSocket>>>);
static_assert(kMoveAssignableOrStableAddress<ws_detail::WsClientTcpUpgradeMachine<ws_detail::WsClientUpgradeState<TcpSocket>>>);

static_assert(!std::is_copy_constructible_v<WsUpgraderImpl<TcpSocket>>);
static_assert(!std::is_copy_assignable_v<WsUpgraderImpl<TcpSocket>>);
static_assert(kMoveConstructibleOrStableAddress<WsUpgraderImpl<TcpSocket>>);
static_assert(kMoveAssignableOrStableAddress<WsUpgraderImpl<TcpSocket>>);

static_assert(!std::is_copy_constructible_v<WsSessionUpgraderImpl<TcpSocket>>);
static_assert(!std::is_copy_assignable_v<WsSessionUpgraderImpl<TcpSocket>>);
static_assert(std::is_nothrow_move_constructible_v<WsSessionUpgraderImpl<TcpSocket>>);
static_assert(std::is_nothrow_move_assignable_v<WsSessionUpgraderImpl<TcpSocket>>);

static_assert(!std::is_copy_constructible_v<WsSessionImpl<TcpSocket>>);
static_assert(!std::is_copy_assignable_v<WsSessionImpl<TcpSocket>>);
static_assert(kMoveConstructibleOrStableAddress<WsSessionImpl<TcpSocket>>);
static_assert(kMoveAssignableOrStableAddress<WsSessionImpl<TcpSocket>>);

static_assert(!std::is_copy_constructible_v<WsFrameBuilder>);
static_assert(!std::is_copy_assignable_v<WsFrameBuilder>);
static_assert(std::is_nothrow_move_constructible_v<WsFrameBuilder>);
static_assert(std::is_nothrow_move_assignable_v<WsFrameBuilder>);
static_assert(BuilderClone<WsFrameBuilder>);

static_assert(std::is_copy_constructible_v<WsFrame>);
static_assert(std::is_copy_assignable_v<WsFrame>);
static_assert(std::is_copy_constructible_v<WsReaderSetting>);
static_assert(std::is_copy_assignable_v<WsReaderSetting>);
static_assert(std::is_copy_constructible_v<WsWriterSetting>);
static_assert(std::is_copy_assignable_v<WsWriterSetting>);
static_assert(std::is_copy_constructible_v<WsClientConfig>);
static_assert(std::is_copy_assignable_v<WsClientConfig>);
static_assert(std::is_copy_constructible_v<WsUrl>);
static_assert(std::is_copy_assignable_v<WsUrl>);
static_assert(std::is_copy_constructible_v<WsError>);
static_assert(std::is_copy_assignable_v<WsError>);
static_assert(std::is_copy_constructible_v<ws_detail::WsFastPathFramePrefix>);
static_assert(std::is_copy_assignable_v<ws_detail::WsFastPathFramePrefix>);
static_assert(std::is_copy_constructible_v<ws_detail::WsFastPathPrefixResult>);
static_assert(std::is_copy_assignable_v<ws_detail::WsFastPathPrefixResult>);
static_assert(std::is_copy_constructible_v<ws_detail::WsConsumeFastPathView>);
static_assert(std::is_copy_assignable_v<ws_detail::WsConsumeFastPathView>);

#ifdef GALAY_SSL_FEATURE_ENABLED
static_assert(!std::is_copy_constructible_v<WsReaderImpl<galay::ssl::SslSocket>>);
static_assert(!std::is_copy_assignable_v<WsReaderImpl<galay::ssl::SslSocket>>);
static_assert(std::is_nothrow_move_constructible_v<WsReaderImpl<galay::ssl::SslSocket>>);
static_assert(std::is_nothrow_move_assignable_v<WsReaderImpl<galay::ssl::SslSocket>>);

static_assert(!std::is_copy_constructible_v<ws_detail::WsRingBufferSslReadMachine<ws_detail::WsFrameReadState>>);
static_assert(!std::is_copy_assignable_v<ws_detail::WsRingBufferSslReadMachine<ws_detail::WsFrameReadState>>);
static_assert(kMoveConstructibleOrStableAddress<ws_detail::WsRingBufferSslReadMachine<ws_detail::WsFrameReadState>>);
static_assert(kMoveAssignableOrStableAddress<ws_detail::WsRingBufferSslReadMachine<ws_detail::WsFrameReadState>>);

static_assert(!std::is_copy_constructible_v<ws_detail::WsSslSendMachine<galay::ssl::SslSocket>>);
static_assert(!std::is_copy_assignable_v<ws_detail::WsSslSendMachine<galay::ssl::SslSocket>>);
static_assert(kMoveConstructibleOrStableAddress<ws_detail::WsSslSendMachine<galay::ssl::SslSocket>>);
static_assert(kMoveAssignableOrStableAddress<ws_detail::WsSslSendMachine<galay::ssl::SslSocket>>);

static_assert(!std::is_copy_constructible_v<WsWriterImpl<galay::ssl::SslSocket>>);
static_assert(!std::is_copy_assignable_v<WsWriterImpl<galay::ssl::SslSocket>>);
static_assert(std::is_nothrow_move_constructible_v<WsWriterImpl<galay::ssl::SslSocket>>);
static_assert(std::is_nothrow_move_assignable_v<WsWriterImpl<galay::ssl::SslSocket>>);

static_assert(!std::is_copy_constructible_v<ws_detail::WsSslEchoMachine<galay::ssl::SslSocket>>);
static_assert(!std::is_copy_assignable_v<ws_detail::WsSslEchoMachine<galay::ssl::SslSocket>>);
static_assert(kMoveConstructibleOrStableAddress<ws_detail::WsSslEchoMachine<galay::ssl::SslSocket>>);
static_assert(kMoveAssignableOrStableAddress<ws_detail::WsSslEchoMachine<galay::ssl::SslSocket>>);

static_assert(!std::is_copy_constructible_v<ws_detail::WsSslEchoLoopMachine<galay::ssl::SslSocket>>);
static_assert(!std::is_copy_assignable_v<ws_detail::WsSslEchoLoopMachine<galay::ssl::SslSocket>>);
static_assert(kMoveConstructibleOrStableAddress<ws_detail::WsSslEchoLoopMachine<galay::ssl::SslSocket>>);
static_assert(kMoveAssignableOrStableAddress<ws_detail::WsSslEchoLoopMachine<galay::ssl::SslSocket>>);

static_assert(!std::is_copy_constructible_v<ws_detail::WsClientUpgradeState<galay::ssl::SslSocket>>);
static_assert(!std::is_copy_assignable_v<ws_detail::WsClientUpgradeState<galay::ssl::SslSocket>>);
static_assert(kMoveConstructibleOrStableAddress<ws_detail::WsClientUpgradeState<galay::ssl::SslSocket>>);
static_assert(kMoveAssignableOrStableAddress<ws_detail::WsClientUpgradeState<galay::ssl::SslSocket>>);

static_assert(!std::is_copy_constructible_v<ws_detail::WsClientSslUpgradeMachine<ws_detail::WsClientUpgradeState<galay::ssl::SslSocket>>>);
static_assert(!std::is_copy_assignable_v<ws_detail::WsClientSslUpgradeMachine<ws_detail::WsClientUpgradeState<galay::ssl::SslSocket>>>);
static_assert(kMoveConstructibleOrStableAddress<ws_detail::WsClientSslUpgradeMachine<ws_detail::WsClientUpgradeState<galay::ssl::SslSocket>>>);
static_assert(kMoveAssignableOrStableAddress<ws_detail::WsClientSslUpgradeMachine<ws_detail::WsClientUpgradeState<galay::ssl::SslSocket>>>);

static_assert(!std::is_copy_constructible_v<WsUpgraderImpl<galay::ssl::SslSocket>>);
static_assert(!std::is_copy_assignable_v<WsUpgraderImpl<galay::ssl::SslSocket>>);
static_assert(kMoveConstructibleOrStableAddress<WsUpgraderImpl<galay::ssl::SslSocket>>);
static_assert(kMoveAssignableOrStableAddress<WsUpgraderImpl<galay::ssl::SslSocket>>);

static_assert(!std::is_copy_constructible_v<WsSessionUpgraderImpl<galay::ssl::SslSocket>>);
static_assert(!std::is_copy_assignable_v<WsSessionUpgraderImpl<galay::ssl::SslSocket>>);
static_assert(std::is_nothrow_move_constructible_v<WsSessionUpgraderImpl<galay::ssl::SslSocket>>);
static_assert(std::is_nothrow_move_assignable_v<WsSessionUpgraderImpl<galay::ssl::SslSocket>>);

static_assert(!std::is_copy_constructible_v<WsSessionImpl<galay::ssl::SslSocket>>);
static_assert(!std::is_copy_assignable_v<WsSessionImpl<galay::ssl::SslSocket>>);
static_assert(kMoveConstructibleOrStableAddress<WsSessionImpl<galay::ssl::SslSocket>>);
static_assert(kMoveAssignableOrStableAddress<WsSessionImpl<galay::ssl::SslSocket>>);

static_assert(std::is_copy_constructible_v<WssClientConfig>);
static_assert(std::is_copy_assignable_v<WssClientConfig>);
#endif

int main()
{
    testBuilderCloneOwnsIndependentFrame();
    std::cout << "T10-WsMoveOnlyContract PASS\n";
    return 0;
}
