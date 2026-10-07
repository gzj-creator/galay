#include <iostream>
#include <string>
#include <string_view>

#include <sstream>

#define private public
#include <galay/cpp/galay-http/kernel/http_writer.h>
#undef private

#include <galay/cpp/galay-kernel/async/async_tcp.h>

int main() {
    using namespace galay::http;
    using namespace galay::async;

    AsyncTcpSocket socket(IPType::IPV4);
    HttpWriterImpl<AsyncTcpSocket> writer(HttpWriterSetting(), socket);

    static constexpr std::string_view kPayload =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 2\r\n"
        "\r\n"
        "OK";

    (void) writer.send_view(kPayload);

    if (writer.get_remaining_bytes() != kPayload.size()) {
        std::cerr << "[T68] sendView should expose full payload length\n";
        return 1;
    }
    if (writer.sent_bytes() != 0) {
        std::cerr << "[T68] sendView should start with zero sent bytes\n";
        return 1;
    }
    if (writer.buffer_data() != kPayload.data()) {
        std::cerr << "[T68] sendView should reference caller-owned storage directly\n";
        return 1;
    }

    writer.update_remaining(5);
    if (writer.get_remaining_bytes() != kPayload.size() - 5) {
        std::cerr << "[T68] partial progress should reduce remaining bytes\n";
        return 1;
    }
    if (writer.sent_bytes() != 5) {
        std::cerr << "[T68] partial progress should advance sent bytes\n";
        return 1;
    }
    if (writer.buffer_data() != kPayload.data()) {
        std::cerr << "[T68] partial progress should keep the external buffer view\n";
        return 1;
    }

    writer.update_remaining(writer.get_remaining_bytes());
    if (writer.get_remaining_bytes() != 0 || writer.sent_bytes() != 0) {
        std::cerr << "[T68] completed sendView should clear pending state\n";
        return 1;
    }
    if (writer.m_external_buffer != nullptr || writer.m_external_buffer_size != 0) {
        std::cerr << "[T68] completed sendView should release external buffer bookkeeping\n";
        return 1;
    }

    std::string owned = "owned-buffer";
    (void) writer.send(owned.data(), owned.size());
    if (writer.get_remaining_bytes() != owned.size()) {
        std::cerr << "[T68] owned send should still work after sendView\n";
        return 1;
    }
    if (std::string(writer.buffer_data(), writer.get_remaining_bytes()) != owned) {
        std::cerr << "[T68] owned send content mismatch after sendView\n";
        return 1;
    }

    std::cout << "T68-HttpWriterSendView PASS\n";
    return 0;
}
