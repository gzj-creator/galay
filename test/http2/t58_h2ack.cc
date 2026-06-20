#include <sstream>

#define private public
#include "galay-http2/kernel/stream_manager.h"
#undef private

#include <cassert>
#include <chrono>
#include <iostream>

using namespace galay::http2;

int main() {
    galay::async::TcpSocket socket(GHandle{-1});
    Http2Conn conn(std::move(socket));
    Http2StreamManager manager(conn);

    conn.runtimeConfig().settings_ack_timeout = std::chrono::seconds(10);
    conn.markSettingsSent();

    const auto sent_at = conn.settingsSentAt();

    manager.m_last_frame_recv_at = sent_at;
    assert(manager.shouldEnforceSettingsAckTimeout(sent_at + std::chrono::seconds(11)));

    manager.m_last_frame_recv_at = sent_at + std::chrono::milliseconds(1);
    assert(!manager.shouldEnforceSettingsAckTimeout(sent_at + std::chrono::seconds(11)));

    conn.markSettingsAckReceived();
    assert(!manager.shouldEnforceSettingsAckTimeout(sent_at + std::chrono::seconds(11)));

    std::cout << "T58-H2SettingsAckTimeout PASS\n";
    return 0;
}
