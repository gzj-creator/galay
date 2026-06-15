/**
 * @file T41-H2CloseTcpTeardown.cc
 * @brief HTTP/2 close path should stay in transport teardown scope
 */

#include "galay-http2/kernel/http2_conn.h"
#include <cerrno>
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
        conn->initiateClose();
        { conn->isClosing() } -> std::same_as<bool>;
        { conn->isGoawaySent() } -> std::same_as<bool>;
        { conn->isGoawayReceived() } -> std::same_as<bool>;
        { conn->isDraining() } -> std::same_as<bool>;
    }, "Http2Conn close contract must expose close/protocol state queries");

    {
        std::cout << "[T41] Scenario 1: initiateClose() only marks closing and keeps protocol flags\n";

        TcpSocket socket(GHandle{-1});
        Http2Conn conn(std::move(socket));

        if (!check(!conn.isClosing(), "new conn should not be closing")) return 1;
        if (!check(!conn.isGoawaySent(), "new conn should not have GOAWAY sent")) return 1;
        if (!check(!conn.isGoawayReceived(), "new conn should not have GOAWAY received")) return 1;
        if (!check(!conn.isDraining(), "new conn should not be draining")) return 1;

        errno = 0;
        conn.initiateClose();

        if (!check(conn.isClosing(), "initiateClose() must set closing")) return 1;
        if (!check(!conn.isGoawaySent(), "initiateClose() must not mark GOAWAY sent")) return 1;
        if (!check(!conn.isGoawayReceived(), "initiateClose() must not mark GOAWAY received")) return 1;
        if (!check(!conn.isDraining(), "initiateClose() must not enter draining state")) return 1;
        if (!check(errno == 0, "initiateClose() should skip TCP shutdown when fd is invalid")) return 1;

        std::cout << "[T41] Scenario 1 PASS: close path stays in transport scope\n";
    }

    {
        std::cout << "[T41] Scenario 2: initiateClose() is idempotent on invalid fd\n";

        TcpSocket socket(GHandle{-1});
        Http2Conn conn(std::move(socket));

        errno = 0;
        conn.initiateClose();
        if (!check(errno == 0, "first initiateClose() should not touch errno for invalid fd")) return 1;

        errno = 0;
        conn.initiateClose();
        if (!check(errno == 0, "second initiateClose() should remain a no-op for invalid fd")) return 1;

        std::cout << "[T41] Scenario 2 PASS: repeated close initiation is safe\n";
    }

    std::cout << "[T41] All scenarios PASS\n";
    return 0;
}
