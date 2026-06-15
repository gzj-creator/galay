/**
 * @file T36-H2TimerBehavior.cc
 * @brief Connection-core timer behavior contract test
 */

#include "galay-http2/kernel/h2_core.h"
#include <cassert>
#include <chrono>
#include <iostream>

using namespace galay::http2;

int main() {
    using namespace std::chrono;
    const auto base = steady_clock::now();

    Http2ConnectionCore core;
    core.setTimerConfig(Http2ConnectionCore::TimerConfig{
        .settings_ack_timeout = 10ms,
        .ping_interval = 5ms,
        .ping_timeout = 10ms,
        .graceful_shutdown_timeout = 20ms
    });

    // SETTINGS ACK timeout
    core.markSettingsSent(base);
    auto e1 = core.checkTimers(base + 11ms);
    assert(e1 == Http2ConnectionCore::TimerEvent::SettingsAckTimeout);

    // PING send + PING ACK timeout
    Http2ConnectionCore core2;
    core2.setTimerConfig(Http2ConnectionCore::TimerConfig{
        .settings_ack_timeout = 10ms,
        .ping_interval = 5ms,
        .ping_timeout = 10ms,
        .graceful_shutdown_timeout = 20ms
    });
    core2.markFrameReceivedAt(base);
    auto e2 = core2.checkTimers(base + 6ms);
    assert(e2 == Http2ConnectionCore::TimerEvent::SendPing);
    auto e3 = core2.checkTimers(base + 17ms);
    assert(e3 == Http2ConnectionCore::TimerEvent::PingAckTimeout);

    // graceful shutdown timeout
    Http2ConnectionCore core3;
    core3.setTimerConfig(Http2ConnectionCore::TimerConfig{
        .settings_ack_timeout = 10ms,
        .ping_interval = 5ms,
        .ping_timeout = 10ms,
        .graceful_shutdown_timeout = 20ms
    });
    core3.beginGracefulShutdown(base);
    auto e4 = core3.checkTimers(base + 21ms);
    assert(e4 == Http2ConnectionCore::TimerEvent::GracefulShutdownTimeout);

    std::cout << "T36-H2TimerBehavior PASS\n";
    return 0;
}
