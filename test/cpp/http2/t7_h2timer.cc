/**
 * @file T36-H2TimerBehavior.cc
 * @brief Connection-core timer behavior contract test
 */

#include <galay/cpp/galay-http2/kernel/h2_core.h>
#include <cassert>
#include <chrono>
#include <iostream>

using namespace galay::http2;

int main() {
    using namespace std::chrono;
    const auto base = steady_clock::now();

    Http2ConnectionCore core;
    core.set_timer_config(Http2ConnectionCore::TimerConfig{
        .settings_ack_timeout = 10ms,
        .ping_interval = 5ms,
        .ping_timeout = 10ms,
        .graceful_shutdown_timeout = 20ms
    });

    // SETTINGS ACK timeout
    core.mark_settings_sent(base);
    auto e1 = core.check_timers(base + 11ms);
    assert(e1 == Http2ConnectionCore::TimerEvent::SettingsAckTimeout);
    assert(!core.has_outbound_work());

    // PING send + PING ACK timeout
    Http2ConnectionCore core2;
    core2.set_timer_config(Http2ConnectionCore::TimerConfig{
        .settings_ack_timeout = 10ms,
        .ping_interval = 5ms,
        .ping_timeout = 10ms,
        .graceful_shutdown_timeout = 20ms
    });
    core2.mark_frame_received_at(base);
    auto e2 = core2.check_timers(base + 6ms);
    assert(e2 == Http2ConnectionCore::TimerEvent::SendPing);
    assert(!core2.has_outbound_work());
    auto e3 = core2.check_timers(base + 17ms);
    assert(e3 == Http2ConnectionCore::TimerEvent::PingAckTimeout);
    assert(!core2.has_outbound_work());

    // graceful shutdown timeout
    Http2ConnectionCore core3;
    core3.set_timer_config(Http2ConnectionCore::TimerConfig{
        .settings_ack_timeout = 10ms,
        .ping_interval = 5ms,
        .ping_timeout = 10ms,
        .graceful_shutdown_timeout = 20ms
    });
    core3.begin_graceful_shutdown(base);
    auto e4 = core3.check_timers(base + 21ms);
    assert(e4 == Http2ConnectionCore::TimerEvent::GracefulShutdownTimeout);
    assert(!core3.has_outbound_work());

    std::cout << "T36-H2TimerBehavior PASS\n";
    return 0;
}
