#include <iostream>
#include <string_view>

#include <sstream>

#define private public
#include <galay/cpp/galay-ws/kernel/ws_reader.h>
#undef private

using galay::utils::RingBuffer;
using namespace galay::websocket;

namespace {

bool writeAll(RingBuffer& ring, std::string_view bytes) {
    return ring.write(bytes.data(), bytes.size()) == bytes.size();
}

bool check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "[T66] " << message << "\n";
        return false;
    }
    return true;
}

} // namespace

int main() {
    WsReaderSetting setting;

    {
        RingBuffer ring(8);
        if (!check(writeAll(ring, "abcdef"), "failed to seed wrapped frame buffer")) {
            return 1;
        }
        ring.consume(4);

        WsFrame frame;
        galay::websocket::detail::WsFrameReadState state(ring, setting, frame, true);
        char* buffer = nullptr;
        size_t length = 0;
        if (!check(state.prepareRecvWindow(buffer, length), "frame state should expose recv window")) {
            return 1;
        }
        if (!check(length == 2, "frame state should expose only first writable segment for SSL recv")) {
            return 1;
        }
        if (!check(!state.m_recv_staged, "frame state should avoid SSL staging scratch on wrapped window")) {
            return 1;
        }
        if (!check(state.m_ssl_recv_scratch.empty(), "frame state should not allocate SSL scratch on wrapped window")) {
            return 1;
        }
    }

    {
        RingBuffer ring(8);
        if (!check(writeAll(ring, "abcdef"), "failed to seed wrapped message buffer")) {
            return 1;
        }
        ring.consume(4);

        std::string message;
        WsOpcode opcode = WsOpcode::Close;
        galay::websocket::detail::WsMessageReadState state(
            ring, setting, message, opcode, true, false, nullptr);
        char* buffer = nullptr;
        size_t length = 0;
        if (!check(state.prepareRecvWindow(buffer, length), "message state should expose recv window")) {
            return 1;
        }
        if (!check(length == 2, "message state should expose only first writable segment for SSL recv")) {
            return 1;
        }
        if (!check(!state.m_recv_staged, "message state should avoid SSL staging scratch on wrapped window")) {
            return 1;
        }
        if (!check(state.m_ssl_recv_scratch.empty(), "message state should not allocate SSL scratch on wrapped window")) {
            return 1;
        }
    }

    std::cout << "T66-WssReaderRecvWindowNoStaging PASS\n";
    return 0;
}
