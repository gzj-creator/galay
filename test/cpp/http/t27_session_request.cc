#include <iostream>
#include <string>

#include <galay/cpp/galay-http/kernel/http_session.h>

int main()
{
    using galay::async::TcpSocket;
    using galay::http::HttpSession;
    using galay::http::detail::HttpSessionState;
    using galay::kernel::IPType;

    TcpSocket socket(IPType::IPV4);
    HttpSession session(socket);

    std::string raw_request =
        "POST /v3/kv/put HTTP/1.1\r\n"
        "Host: 127.0.0.1:2379\r\n"
        "Accept: application/json\r\n"
        "Connection: keep-alive\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 13\r\n"
        "\r\n"
        "{\"k\":\"v\"}";

    HttpSessionState<TcpSocket> state(session, std::move(raw_request));

    if (state.sendRemaining() == 0) {
        std::cerr << "[T78] serialized request should expose pending bytes\n";
        return 1;
    }
    if (std::string(state.sendBuffer(), state.sendRemaining()).find("POST /v3/kv/put HTTP/1.1\r\n") != 0) {
        std::cerr << "[T78] serialized request should be preserved verbatim\n";
        return 1;
    }
    if (state.sendCompleted()) {
        std::cerr << "[T78] serialized request should not start completed\n";
        return 1;
    }

    state.onBytesSent(state.sendRemaining());
    if (!state.sendCompleted()) {
        std::cerr << "[T78] serialized request should finish after all bytes are sent\n";
        return 1;
    }

    std::cout << "T78-HttpSessionSerializedRequest PASS\n";
    return 0;
}
