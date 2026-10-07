#include <cstring>
#include <iostream>
#include <string>
#include <type_traits>

namespace test {
struct FakeTcpSocket {};
}

#include <sstream>

#define private public
#include <galay/cpp/galay-ws/kernel/ws_writer.h>
#undef private

#include <galay/cpp/galay-ws/protoc/ws_frame.h>

namespace galay::websocket {
template<>
struct is_tcp_socket<test::FakeTcpSocket> : std::true_type {};
}

using namespace galay::websocket;

namespace {

bool check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "[T61] " << message << "\n";
        return false;
    }
    return true;
}

std::string flatten_io_vecs(const WsWriterImpl<test::FakeTcpSocket>& writer) {
    const auto* iovecs = writer.get_iovecs_data();
    const size_t count = writer.get_iovecs_count();
    std::string result;
    result.reserve(writer.get_remaining_bytes());
    for (size_t index = 0; index < count; ++index) {
        const auto& seg = iovecs[index];
        result.append(static_cast<const char*>(seg.iov_base), seg.iov_len);
    }
    return result;
}

} // namespace

int main() {
    auto fail = [&](const char* message) {
        std::cerr << "[T61] " << message << "\n";
        return 1;
    };

    using Writer = WsWriterImpl<test::FakeTcpSocket>;
    WsWriterSetting setting = WsWriterSetting::by_server();
    test::FakeTcpSocket socket;
    Writer writer(setting, socket);

    const std::string text_payload = "fast-path text";
    const std::string binary_payload(32, '\xA5');

    const auto text_frame = WsFrameParser::create_text_frame(text_payload);
    const auto expected_text = WsFrameParser::to_bytes(text_frame, setting.use_mask);
    const auto hits_before_text = writer.m_fast_path_counters.hits;
    const auto fallbacks_before_text = writer.m_fast_path_counters.fallbacks;

    writer.reset_pending_state();
    writer.prepare_send_frame(text_frame);
    const auto flat_text = flatten_io_vecs(writer);
    if (!check(flat_text == expected_text, "text send layout must match WsFrameParser::toBytes")) {
        return fail("text layout mismatch");
    }
    if (!check(writer.m_fast_path_counters.hits == hits_before_text + 1,
               "text send should increment fast-path hits")) {
        return fail("text fast-path hit missing");
    }
    if (!check(writer.m_fast_path_counters.fallbacks == fallbacks_before_text,
               "text send should not count as fallback")) {
        return fail("text fallback regression");
    }

    const auto binary_frame = WsFrameParser::create_binary_frame(binary_payload);
    const auto expected_binary = WsFrameParser::to_bytes(binary_frame, setting.use_mask);
    const auto hits_before_binary = writer.m_fast_path_counters.hits;
    const auto fallbacks_before_binary = writer.m_fast_path_counters.fallbacks;

    writer.reset_pending_state();
    writer.prepare_send_frame(binary_frame);
    const auto flat_binary = flatten_io_vecs(writer);
    if (!check(flat_binary == expected_binary, "binary send layout must match WsFrameParser::toBytes")) {
        return fail("binary layout mismatch");
    }
    if (!check(writer.m_fast_path_counters.hits == hits_before_binary + 1,
               "binary send should increment fast-path hits")) {
        return fail("binary fast-path hit missing");
    }
    if (!check(writer.m_fast_path_counters.fallbacks == fallbacks_before_binary,
               "binary send should not count as fallback")) {
        return fail("binary fallback regression");
    }

    const auto ping_frame = WsFrameParser::create_ping_frame("ping");
    const auto hits_before_ping = writer.m_fast_path_counters.hits;
    const auto fallbacks_before_ping = writer.m_fast_path_counters.fallbacks;

    writer.reset_pending_state();
    writer.prepare_send_frame(ping_frame);
    if (!check(writer.m_fast_path_counters.hits == hits_before_ping,
               "ping should not increment fast-path hits")) {
        return fail("ping fast-path hit regression");
    }
    if (!check(writer.m_fast_path_counters.fallbacks == fallbacks_before_ping + 1,
               "ping should count as fallback")) {
        return fail("ping fallback regression");
    }

    std::cout << "T61-WsWriterCommonFastPath PASS\n";
    return 0;
}
