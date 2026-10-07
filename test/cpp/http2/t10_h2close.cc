/**
 * @file T41-H2CloseTcpTeardown.cc
 * @brief HTTP/2 close path should stay in transport teardown scope
 */

#include <galay/cpp/galay-http2/kernel/h2_core.h>
#include <galay/cpp/galay-http2/kernel/http2_conn.h>
#include <cerrno>
#include <chrono>
#include <iostream>

using namespace galay::http2;
using namespace galay::async;

int main() {
    std::cout << "[T41] Starting HTTP/2 close TCP teardown contract tests\n";

    auto check = [](bool cond, const char* msg) {
        if (!cond) {
            std::cerr << "[T41] CHECK FAILED: " << msg << "\n";
            return false;
        }
        return true;
    };

    static_assert(requires(Http2Conn* conn) {
        conn->initiate_close();
        { conn->is_closing() } -> std::same_as<bool>;
        { conn->is_goaway_sent() } -> std::same_as<bool>;
        { conn->is_goaway_received() } -> std::same_as<bool>;
        { conn->is_draining() } -> std::same_as<bool>;
    }, "Http2Conn close contract must expose close/protocol state queries");

    {
        std::cout << "[T41] Scenario 1: initiateClose() only marks closing and keeps protocol flags\n";

        AsyncTcpSocket socket(GHandle{-1});
        Http2Conn conn(std::move(socket));

        if (!check(!conn.is_closing(), "new conn should not be closing")) return 1;
        if (!check(!conn.is_goaway_sent(), "new conn should not have GOAWAY sent")) return 1;
        if (!check(!conn.is_goaway_received(), "new conn should not have GOAWAY received")) return 1;
        if (!check(!conn.is_draining(), "new conn should not be draining")) return 1;

        errno = 0;
        conn.initiate_close();

        if (!check(conn.is_closing(), "initiateClose() must set closing")) return 1;
        if (!check(!conn.is_goaway_sent(), "initiateClose() must not mark GOAWAY sent")) return 1;
        if (!check(!conn.is_goaway_received(), "initiateClose() must not mark GOAWAY received")) return 1;
        if (!check(!conn.is_draining(), "initiateClose() must not enter draining state")) return 1;
        if (!check(errno == 0, "initiateClose() should skip TCP shutdown when fd is invalid")) return 1;

        std::cout << "[T41] Scenario 1 PASS: close path stays in transport scope\n";
    }

    {
        std::cout << "[T41] Scenario 2: initiateClose() is idempotent on invalid fd\n";

        AsyncTcpSocket socket(GHandle{-1});
        Http2Conn conn(std::move(socket));

        errno = 0;
        conn.initiate_close();
        if (!check(errno == 0, "first initiateClose() should not touch errno for invalid fd")) return 1;

        errno = 0;
        conn.initiate_close();
        if (!check(errno == 0, "second initiateClose() should remain a no-op for invalid fd")) return 1;

        std::cout << "[T41] Scenario 2 PASS: repeated close initiation is safe\n";
    }

    {
        std::cout << "[T41] Scenario 3: connection core exposes draining and forced close states\n";

        using namespace std::chrono;
        const auto base = steady_clock::now();
        Http2ConnectionCore core;
        core.set_timer_config(Http2ConnectionCore::TimerConfig{
            .settings_ack_timeout = 10ms,
            .ping_interval = 0ms,
            .ping_timeout = 10ms,
            .graceful_shutdown_timeout = 20ms
        });

        core.begin_graceful_shutdown(base);
        if (!check(core.state() == Http2ConnectionCore::State::Draining,
                   "beginGracefulShutdown() must enter draining state")) return 1;
        if (!check(!core.accepts_new_streams(),
                   "draining core must reject new streams")) return 1;

        auto timeout = core.check_timers(base + 21ms);
        if (!check(timeout == Http2ConnectionCore::TimerEvent::GracefulShutdownTimeout,
                   "graceful shutdown timeout should be reported")) return 1;
        core.apply_timer_event(timeout);
        if (!check(core.state() == Http2ConnectionCore::State::Closing,
                   "graceful timeout must force closing state")) return 1;
        if (!check(core.stop_requested(),
                   "forced close must request run loop stop")) return 1;

        std::cout << "[T41] Scenario 3 PASS: core close states are explicit\n";
    }

    std::cout << "[T41] All scenarios PASS\n";
    return 0;
}
