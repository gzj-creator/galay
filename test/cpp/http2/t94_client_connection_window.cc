#include <array>
#include <cassert>
#include <sstream>
#define private public
#include <galay/cpp/galay-http2/kernel/stream_manager.h>
#undef private

using namespace galay::http2;
using namespace galay::async;

int main()
{
    AsyncTcpSocket socket(GHandle{-1});
    Http2Conn conn(std::move(socket));
    conn.set_is_client(true);
    conn.runtime_config().flow_control_target_window = 1024 * 1024;
    Http2StreamManager manager(conn);
    manager.prepare_for_start(false);
    assert(conn.conn_recv_window() == 1024 * 1024);
    auto update = manager.m_send_channel.try_recv();
    assert(update);
    const auto bytes = update->flatten();
    const auto parsed = Http2FrameParser::parse_frame(
        reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
    assert(parsed && (*parsed)->is_window_update());
    assert((*parsed)->stream_id() == 0);
    assert((*parsed)->as_window_update()->window_size_increment() ==
        1024 * 1024 - kDefaultInitialWindowSize);
    return 0;
}
