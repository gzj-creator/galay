#include <iostream>
// ws_conn.h pulls in standard headers; include sstream before redefining private.
#include <sstream>
#include <utility>

#ifdef GALAY_SSL_FEATURE_ENABLED
#define private public
#include "galay-ws/kernel/ws_conn.h"
#undef private
#include "galay-ssl/async/ssl_socket.h"
#endif

namespace {

bool check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "[T68] " << message << "\n";
        return false;
    }
    return true;
}

#ifdef GALAY_SSL_FEATURE_ENABLED
using LoopMachine = galay::websocket::detail::WsSslEchoLoopMachine<galay::ssl::SslSocket>;

LoopMachine makeMovedLoopMachine(galay::websocket::WssConn& conn) {
    LoopMachine machine(
        &conn,
        galay::websocket::WsReaderSetting(),
        galay::websocket::WsWriterSetting::byServer());
    return std::move(machine);
}
#endif

} // namespace

int main() {
#ifndef GALAY_SSL_FEATURE_ENABLED
    std::cout << "T68-WssLoopMove SKIP\n";
    return 0;
#else
    galay::ssl::SslSocket socket(nullptr);
    galay::websocket::WssConn conn(std::move(socket), true);

    auto machine = makeMovedLoopMachine(conn);

    if (!check(machine.m_read_state.m_message == &machine.m_message,
               "moved loop machine should rebind reader message storage")) {
        return 1;
    }
    if (!check(machine.m_read_state.m_opcode == &machine.m_opcode,
               "moved loop machine should rebind reader opcode storage")) {
        return 1;
    }

    std::cout << "T68-WssLoopMove PASS\n";
    return 0;
#endif
}
