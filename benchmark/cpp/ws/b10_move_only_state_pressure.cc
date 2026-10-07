#include "../common/benchmark_environment.h"

#include <galay/cpp/galay-ws/builder/ws_frame_builder.h>
#include <galay/cpp/galay-ws/client/ws_client.h>
#include <galay/cpp/galay-ws/kernel/ws_conn.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <string>

using galay::async::AsyncTcpSocket;
using galay::utils::RingBuffer;
using namespace galay::websocket;

namespace {

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "[benchmark_ws_move_only_state_pressure] " << message << "\n";
        std::abort();
    }
}

size_t parse_iterations(int argc, char** argv)
{
    constexpr size_t kDefaultIterations = 10000;
    if (argc < 2) {
        return kDefaultIterations;
    }

    char* end = nullptr;
    errno = 0;
    const unsigned long long value = std::strtoull(argv[1], &end, 10);
    if (errno != 0 || end == argv[1] || *end != '\0' || value == 0 ||
        value > static_cast<unsigned long long>(std::numeric_limits<size_t>::max())) {
        return kDefaultIterations;
    }
    return static_cast<size_t>(value);
}

template <typename Func>
void run_bench(const char* name, size_t iterations, Func&& func)
{
    const auto start = std::chrono::steady_clock::now();
    size_t accepted = 0;
    for (size_t i = 0; i < iterations; ++i) {
        accepted += func(i) ? 1 : 0;
    }
    const auto end = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(end - start).count();
    std::cout << name << ": " << iterations << " iterations, "
              << (static_cast<double>(iterations) / seconds) << " ops/s, accepted="
              << accepted << "\n";
}

WsUrl make_local_url()
{
    WsUrl url;
    url.scheme = "ws";
    url.host = "127.0.0.1";
    url.port = 80;
    url.path = "/ws";
    url.is_secure = false;
    return url;
}

bool exercise_builder_clone_move(size_t iteration)
{
    std::string payload(256 + (iteration % 31), 'a');
    payload[0] = static_cast<char>('a' + (iteration % 26));

    WsFrameBuilder source;
    source.binary(payload);
    WsFrameBuilder cloned = source.clone();
    WsFrameBuilder moved = std::move(source);

    WsFrame clone_frame = cloned.build_move();
    WsFrame moved_frame = moved.build_move();
    return clone_frame.payload == payload &&
           moved_frame.payload == payload &&
           clone_frame.payload.data() != moved_frame.payload.data();
}

bool exercise_reader_writer_move(size_t iteration)
{
    AsyncTcpSocket socket;
    RingBuffer ring(4096);

    WsReaderSetting reader_setting;
    WsReaderImpl<AsyncTcpSocket> reader(ring, reader_setting, socket, true, false);
    WsReaderImpl<AsyncTcpSocket> moved_reader(std::move(reader));

    const std::string payload(64 + (iteration % 17), 'x');
    WsWriterImpl<AsyncTcpSocket> writer(WsWriterSetting::by_server(), socket);
    auto send_operation = writer.send_text(payload);
    const bool operation_ready = send_operation.await_ready();
    WsWriterImpl<AsyncTcpSocket> moved_writer(std::move(writer));

    return !operation_ready &&
           moved_writer.get_remaining_bytes() >= payload.size() &&
           moved_writer.get_iovecs_data() != nullptr &&
           moved_writer.get_iovecs_count() > 0;
}

bool exercise_session_and_upgrader_state(size_t)
{
    AsyncTcpSocket socket;
    RingBuffer ring(4096);
    WsUrl url = make_local_url();
    WsReaderSetting reader_setting;
    WsWriterSetting writer_setting = WsWriterSetting::by_client();
    std::unique_ptr<WsConnImpl<AsyncTcpSocket>> ws_conn;

    WsSessionImpl<AsyncTcpSocket> session(socket, url, writer_setting, 4096, reader_setting);
    WsSessionUpgraderImpl<AsyncTcpSocket> session_upgrader = session.upgrade();
    WsSessionUpgraderImpl<AsyncTcpSocket> moved_session_upgrader(std::move(session_upgrader));

    WsUpgraderImpl<AsyncTcpSocket> client_upgrader(
        &socket,
        &ring,
        url,
        reader_setting,
        writer_setting,
        &ws_conn);
    WsUpgraderImpl<AsyncTcpSocket> moved_client_upgrader(std::move(client_upgrader));

    return !session.is_upgraded();
}

} // namespace

int main(int argc, char** argv)
{
    if (!galay::benchmark::initialize_benchmark_environment()) {
        return 1;
    }

    const size_t iterations = parse_iterations(argc, argv);

    require(exercise_builder_clone_move(0), "builder clone/move fixture should pass");
    require(exercise_reader_writer_move(0), "reader/writer move fixture should pass");
    require(exercise_session_and_upgrader_state(0), "session/upgrader fixture should pass");

    run_bench("BM_BuilderCloneMove", iterations, exercise_builder_clone_move);
    run_bench("BM_ReaderWriterMove", iterations, exercise_reader_writer_move);
    run_bench("BM_SessionUpgraderState", iterations, exercise_session_and_upgrader_state);

    return 0;
}
