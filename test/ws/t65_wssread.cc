#include <iostream>

#ifdef GALAY_SSL_FEATURE_ENABLED
#include <sstream>

#define private public
#include "ws/kernel/ws_reader.h"
#undef private
#include "ssl/async/ssl_socket.h"
#endif

int main() {
#ifdef GALAY_SSL_FEATURE_ENABLED
    using galay::async::TcpSocket;
    using galay::utils::RingBuffer;
    using namespace galay::websocket;

    RingBuffer ring(1024);
    WsReaderSetting setting;

    galay::ssl::SslSocket ssl_socket(nullptr);
    WsReaderImpl<galay::ssl::SslSocket> wss_reader(ring, setting, ssl_socket, true, false);
    if (!wss_reader.messageFastPathEnabled()) {
        std::cerr << "[T65] WSS reader should keep message fast path enabled\n";
        return 1;
    }

    TcpSocket tcp_socket;
    WsReaderImpl<TcpSocket> ws_reader(ring, setting, tcp_socket, true, false);
    if (!ws_reader.messageFastPathEnabled()) {
        std::cerr << "[T65] WS reader should keep message fast path enabled\n";
        return 1;
    }
#endif

    std::cout << "T65-WssReaderDispatchFastPath PASS\n";
    return 0;
}
