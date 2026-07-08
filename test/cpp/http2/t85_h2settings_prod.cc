#include <iostream>
#include <memory>
#include <string_view>
#include <atomic>
#include <chrono>
#include <thread>
#include <unistd.h>

#define private public
#include <galay/cpp/galay-http2/client/h2_client.h>
#include <galay/cpp/galay-http2/client/h2c_client.h>
#include <galay/cpp/galay-http2/kernel/stream_manager.h>
#include <galay/cpp/galay-http2/server/http2_server.h>
#include <galay/cpp/galay-kernel/core/runtime.h>
#undef private

#include <galay/cpp/galay-utils/encoding/base64.hpp>

using namespace galay::http2;
using namespace galay::kernel;

namespace {

std::atomic<bool> g_upgrade_done{false};
std::atomic<bool> g_upgrade_ok{false};

bool check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        return false;
    }
    return true;
}

Http2SettingsFrame settingsFrame(Http2SettingsId id, uint32_t value) {
    Http2SettingsFrame frame;
    frame.addSetting(id, value);
    return frame;
}

bool assertSettingsValueValidation() {
    bool ok = true;
    Http2Settings settings;

    ok &= check(settings.applySettings(
        settingsFrame(Http2SettingsId::EnablePush, 2)) == Http2ErrorCode::ProtocolError,
        "ENABLE_PUSH > 1 must be ProtocolError");
    ok &= check(settings.applySettings(
        settingsFrame(Http2SettingsId::InitialWindowSize, 2147483648u)) ==
        Http2ErrorCode::FlowControlError,
        "INITIAL_WINDOW_SIZE > 2^31 - 1 must be FlowControlError");
    ok &= check(settings.applySettings(
        settingsFrame(Http2SettingsId::MaxFrameSize, kMinFrameSize - 1)) ==
        Http2ErrorCode::ProtocolError,
        "MAX_FRAME_SIZE below minimum must be ProtocolError");
    ok &= check(settings.applySettings(
        settingsFrame(Http2SettingsId::MaxFrameSize, kMaxFrameSize + 1)) ==
        Http2ErrorCode::ProtocolError,
        "MAX_FRAME_SIZE above maximum must be ProtocolError");

    Http2SettingsFrame duplicate;
    duplicate.addSetting(Http2SettingsId::MaxFrameSize, kMinFrameSize);
    duplicate.addSetting(Http2SettingsId::MaxFrameSize, kMinFrameSize + 4096);
    ok &= check(settings.applySettings(duplicate) == Http2ErrorCode::NoError,
                "duplicate SETTINGS must apply successfully");
    ok &= check(settings.max_frame_size == kMinFrameSize + 4096,
                "duplicate SETTINGS must keep the last value");
    return ok;
}

bool assertSettingsFrameValidation() {
    bool ok = true;
    galay::async::TcpSocket socket(GHandle{-1});
    Http2Conn conn(std::move(socket));
    Http2StreamManager manager(conn);

    conn.markSettingsSent();

    auto ack_with_payload = std::make_unique<Http2SettingsFrame>();
    ack_with_payload->setAck(true);
    ack_with_payload->addSetting(Http2SettingsId::HeaderTableSize, 1);
    manager.handleConnectionFrame(std::move(ack_with_payload));
    manager.processPendingActions();

    auto invalid_ack_response = manager.m_send_channel.tryRecv();
    ok &= check(invalid_ack_response.has_value(),
                "ACK SETTINGS with payload must enqueue GOAWAY");
    ok &= check(invalid_ack_response && invalid_ack_response->frame != nullptr,
                "invalid ACK SETTINGS response must be a frame");
    ok &= check(invalid_ack_response && invalid_ack_response->frame &&
                invalid_ack_response->frame->isGoAway(),
                "invalid ACK SETTINGS response must be GOAWAY");
    ok &= check(invalid_ack_response && invalid_ack_response->frame &&
                invalid_ack_response->frame->asGoAway()->errorCode() ==
                    Http2ErrorCode::FrameSizeError,
                "ACK SETTINGS with payload must be FrameSizeError");
    ok &= check(conn.isSettingsAckPending(),
                "invalid ACK SETTINGS must not clear pending SETTINGS ACK state");

    auto nonzero_stream_settings = std::make_unique<Http2SettingsFrame>();
    nonzero_stream_settings->header().stream_id = 1;
    nonzero_stream_settings->addSetting(Http2SettingsId::HeaderTableSize, 128);
    manager.handleConnectionFrame(std::move(nonzero_stream_settings));
    manager.processPendingActions();

    auto invalid_stream_response = manager.m_send_channel.tryRecv();
    ok &= check(invalid_stream_response.has_value(),
                "SETTINGS on nonzero stream id must enqueue GOAWAY");
    ok &= check(invalid_stream_response && invalid_stream_response->frame != nullptr,
                "nonzero stream SETTINGS response must be a frame");
    ok &= check(invalid_stream_response && invalid_stream_response->frame &&
                invalid_stream_response->frame->isGoAway(),
                "nonzero stream SETTINGS response must be GOAWAY");
    ok &= check(invalid_stream_response && invalid_stream_response->frame &&
                invalid_stream_response->frame->asGoAway()->errorCode() ==
                    Http2ErrorCode::ProtocolError,
                "SETTINGS on nonzero stream id must be ProtocolError");
    return ok;
}

bool assertHpackLimitsFollowSettings() {
    bool ok = true;
    galay::async::TcpSocket socket(GHandle{-1});
    Http2Conn conn(std::move(socket));

    Http2SettingsFrame local_limits;
    local_limits.addSetting(Http2SettingsId::HeaderTableSize, 128);
    local_limits.addSetting(Http2SettingsId::MaxHeaderListSize, 64);

    ok &= check(conn.applyLocalSettings(local_limits) == Http2ErrorCode::NoError,
                "local SETTINGS must apply successfully");
    ok &= check(conn.localSettings().header_table_size == 128,
                "local HEADER_TABLE_SIZE must update local settings");
    ok &= check(conn.localSettings().max_header_list_size == 64,
                "local MAX_HEADER_LIST_SIZE must update local settings");
    ok &= check(conn.decoder().dynamicTable().maxSize() == 128,
                "local HEADER_TABLE_SIZE must update decoder table size");
    ok &= check(conn.decoder().maxHeaderListSize() == 64,
                "local MAX_HEADER_LIST_SIZE must update decoder limit");

    Http2SettingsFrame peer_limits;
    peer_limits.addSetting(Http2SettingsId::HeaderTableSize, 256);

    ok &= check(conn.applyPeerSettings(peer_limits) == Http2ErrorCode::NoError,
                "peer SETTINGS must apply successfully");
    ok &= check(conn.peerSettings().header_table_size == 256,
                "peer HEADER_TABLE_SIZE must update peer settings");
    ok &= check(conn.encoder().dynamicTable().maxSize() == 256,
                "peer HEADER_TABLE_SIZE must update encoder table size");
    return ok;
}

std::string encodeH2cSettingsHeader(const Http2SettingsFrame& frame) {
    std::string serialized = frame.serialize();
    std::string base64_settings = galay::utils::Base64Util::Base64Encode(
        reinterpret_cast<const unsigned char*>(serialized.data() + kHttp2FrameHeaderLength),
        serialized.size() - kHttp2FrameHeaderLength);
    for (char& c : base64_settings) {
        if (c == '+') {
            c = '-';
        } else if (c == '/') {
            c = '_';
        }
    }
    base64_settings.erase(
        std::remove(base64_settings.begin(), base64_settings.end(), '='),
        base64_settings.end());
    return base64_settings;
}

bool assertBuilderConfigNormalization() {
    bool ok = true;

    auto h2c_client = H2cClientBuilder()
        .initialWindowSize(2147483648u)
        .maxFrameSize(kMinFrameSize - 1)
        .buildConfig();
    ok &= check(h2c_client.initial_window_size == 2147483647u,
                "H2cClientBuilder must clamp INITIAL_WINDOW_SIZE");
    ok &= check(h2c_client.max_frame_size == kMinFrameSize,
                "H2cClientBuilder must clamp MAX_FRAME_SIZE");

#ifdef GALAY_SSL_FEATURE_ENABLED
    auto h2_client = H2ClientBuilder()
        .initialWindowSize(2147483648u)
        .maxFrameSize(kMaxFrameSize + 1)
        .buildConfig();
    ok &= check(h2_client.initial_window_size == 2147483647u,
                "H2ClientBuilder must clamp INITIAL_WINDOW_SIZE");
    ok &= check(h2_client.max_frame_size == kMaxFrameSize,
                "H2ClientBuilder must clamp MAX_FRAME_SIZE");
#endif

    auto h2c_server = H2cServerBuilder()
        .initialWindowSize(2147483648u)
        .maxFrameSize(kMinFrameSize - 1)
        .buildConfig();
    ok &= check(h2c_server.initial_window_size == 2147483647u,
                "H2cServerBuilder must clamp INITIAL_WINDOW_SIZE");
    ok &= check(h2c_server.max_frame_size == kMinFrameSize,
                "H2cServerBuilder must clamp MAX_FRAME_SIZE");

#ifdef GALAY_SSL_FEATURE_ENABLED
    auto h2_server = H2ServerBuilder()
        .initialWindowSize(2147483648u)
        .maxFrameSize(kMaxFrameSize + 1)
        .buildConfig();
    ok &= check(h2_server.initial_window_size == 2147483647u,
                "H2ServerBuilder must clamp INITIAL_WINDOW_SIZE");
    ok &= check(h2_server.max_frame_size == kMaxFrameSize,
                "H2ServerBuilder must clamp MAX_FRAME_SIZE");
#endif

    return ok;
}

bool assertH2cUpgradeSettingsDecode() {
    bool ok = true;

    Http2SettingsFrame upgrade_settings;
    upgrade_settings.addSetting(Http2SettingsId::HeaderTableSize, 128);
    upgrade_settings.addSetting(Http2SettingsId::InitialWindowSize, 65536);
    upgrade_settings.addSetting(Http2SettingsId::MaxFrameSize, kMinFrameSize + 1024);
    auto decoded = Http2Conn::decodeH2cUpgradeSettingsHeader(
        encodeH2cSettingsHeader(upgrade_settings));
    ok &= check(decoded.has_value(),
                "HTTP2-Settings header must decode successfully");
    ok &= check(decoded && decoded->settings().size() == 3,
                "decoded HTTP2-Settings must preserve all settings");
    ok &= check(decoded && decoded->settings()[0].id == Http2SettingsId::HeaderTableSize &&
                    decoded->settings()[0].value == 128,
                "decoded HTTP2-Settings must preserve HEADER_TABLE_SIZE");
    ok &= check(decoded && decoded->settings()[1].id == Http2SettingsId::InitialWindowSize &&
                    decoded->settings()[1].value == 65536,
                "decoded HTTP2-Settings must preserve INITIAL_WINDOW_SIZE");
    ok &= check(decoded && decoded->settings()[2].id == Http2SettingsId::MaxFrameSize &&
                    decoded->settings()[2].value == kMinFrameSize + 1024,
                "decoded HTTP2-Settings must preserve MAX_FRAME_SIZE");

    auto invalid = Http2Conn::decodeH2cUpgradeSettingsHeader("%%%");
    ok &= check(!invalid.has_value(),
                "invalid HTTP2-Settings header must fail decoding");
    ok &= check(!invalid.has_value() && invalid.error() == Http2ErrorCode::ProtocolError,
                "invalid HTTP2-Settings header must report ProtocolError");
    return ok;
}

Task<void> noopStreamHandler(Http2Stream::ptr) {
    co_return;
}

Task<void> runH2cUpgradeClient(uint16_t port) {
    H2cClient<> client(H2cClientBuilder().build());

    auto connect_result = co_await client.connect("127.0.0.1", port);
    if (!connect_result) {
        g_upgrade_done = true;
        co_return;
    }

    auto upgrade_result = co_await client.upgrade("/settings");
    if (!upgrade_result) {
        g_upgrade_done = true;
        co_return;
    }

    auto* conn = client.getConn();
    if (conn != nullptr) {
        const auto& peer = conn->peerSettings();
        g_upgrade_ok = peer.max_concurrent_streams == 17 &&
                       peer.initial_window_size == 70000 &&
                       peer.max_frame_size == kMinFrameSize + 2048 &&
                       peer.max_header_list_size == 1234 &&
                       peer.enable_push == 0;
    }

    auto shutdown_result = co_await client.shutdown();
    if (!shutdown_result) {
        g_upgrade_ok = false;
    }

    g_upgrade_done = true;
    co_return;
}

bool assertH2cUpgradeAppliesPeerSettings() {
    g_upgrade_done = false;
    g_upgrade_ok = false;

    const uint16_t port = static_cast<uint16_t>(22000 + (::getpid() % 10000));
    H2cServer server(H2cServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .ioSchedulerCount(1)
        .computeSchedulerCount(0)
        .maxConcurrentStreams(17)
        .initialWindowSize(70000)
        .maxFrameSize(kMinFrameSize + 2048)
        .maxHeaderListSize(1234)
        .enablePush(false)
        .streamHandler(noopStreamHandler)
        .build());

    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    runtime.start();
    auto* scheduler = runtime.getNextIOScheduler();
    if (scheduler == nullptr) {
        runtime.stop();
        server.stop();
        return false;
    }

    if (!scheduleTask(scheduler, runH2cUpgradeClient(port))) {
        runtime.stop();
        server.stop();
        return false;
    }

    for (int i = 0; i < 100; ++i) {
        if (g_upgrade_done.load(std::memory_order_acquire)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    runtime.stop();
    server.stop();

    bool ok = true;
    ok &= check(g_upgrade_done.load(std::memory_order_acquire),
                "h2c upgrade peer SETTINGS test must complete");
    ok &= check(g_upgrade_ok.load(std::memory_order_acquire),
                "h2c client upgrade must apply peer SETTINGS to the finalized connection");
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok &= assertSettingsValueValidation();
    ok &= assertSettingsFrameValidation();
    ok &= assertHpackLimitsFollowSettings();
    ok &= assertBuilderConfigNormalization();
    ok &= assertH2cUpgradeSettingsDecode();
    ok &= assertH2cUpgradeAppliesPeerSettings();
    if (!ok) {
        return 1;
    }

    std::cout << "T85-H2SettingsProd PASS\n";
    return 0;
}
