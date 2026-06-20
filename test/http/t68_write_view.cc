#include <iostream>
#include <string>
#include <string_view>

#include <sstream>

#define private public
#include <galay/cpp/galay-http/kernel/http_writer.h>
#undef private

#include <galay/cpp/galay-kernel/async/tcp_socket.h>

int main() {
    using namespace galay::http;
    using namespace galay::async;

    TcpSocket socket(IPType::IPV4);
    HttpWriterImpl<TcpSocket> writer(HttpWriterSetting(), socket);

    static constexpr std::string_view kPayload =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 2\r\n"
        "\r\n"
        "OK";

    (void) writer.sendView(kPayload);

    if (writer.getRemainingBytes() != kPayload.size()) {
        std::cerr << "[T68] sendView should expose full payload length\n";
        return 1;
    }
    if (writer.sentBytes() != 0) {
        std::cerr << "[T68] sendView should start with zero sent bytes\n";
        return 1;
    }
    if (writer.bufferData() != kPayload.data()) {
        std::cerr << "[T68] sendView should reference caller-owned storage directly\n";
        return 1;
    }

    writer.updateRemaining(5);
    if (writer.getRemainingBytes() != kPayload.size() - 5) {
        std::cerr << "[T68] partial progress should reduce remaining bytes\n";
        return 1;
    }
    if (writer.sentBytes() != 5) {
        std::cerr << "[T68] partial progress should advance sent bytes\n";
        return 1;
    }
    if (writer.bufferData() != kPayload.data()) {
        std::cerr << "[T68] partial progress should keep the external buffer view\n";
        return 1;
    }

    writer.updateRemaining(writer.getRemainingBytes());
    if (writer.getRemainingBytes() != 0 || writer.sentBytes() != 0) {
        std::cerr << "[T68] completed sendView should clear pending state\n";
        return 1;
    }
    if (writer.m_external_buffer != nullptr || writer.m_external_buffer_size != 0) {
        std::cerr << "[T68] completed sendView should release external buffer bookkeeping\n";
        return 1;
    }

    std::string owned = "owned-buffer";
    (void) writer.send(owned.data(), owned.size());
    if (writer.getRemainingBytes() != owned.size()) {
        std::cerr << "[T68] owned send should still work after sendView\n";
        return 1;
    }
    if (std::string(writer.bufferData(), writer.getRemainingBytes()) != owned) {
        std::cerr << "[T68] owned send content mismatch after sendView\n";
        return 1;
    }

    std::cout << "T68-HttpWriterSendView PASS\n";
    return 0;
}
