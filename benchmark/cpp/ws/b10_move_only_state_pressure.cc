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

using galay::async::TcpSocket;
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

size_t parseIterations(int argc, char** argv)
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
void runBench(const char* name, size_t iterations, Func&& func)
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

WsUrl makeLocalUrl()
{
    WsUrl url;
    url.scheme = "ws";
    url.host = "127.0.0.1";
    url.port = 80;
    url.path = "/ws";
    url.is_secure = false;
    return url;
}

bool exerciseBuilderCloneMove(size_t iteration)
{
    std::string payload(256 + (iteration % 31), 'a');
    payload[0] = static_cast<char>('a' + (iteration % 26));

    WsFrameBuilder source;
    source.binary(payload);
    WsFrameBuilder cloned = source.clone();
    WsFrameBuilder moved = std::move(source);

    WsFrame clone_frame = cloned.buildMove();
    WsFrame moved_frame = moved.buildMove();
    return clone_frame.payload == payload &&
           moved_frame.payload == payload &&
           clone_frame.payload.data() != moved_frame.payload.data();
}

bool exerciseReaderWriterMove(size_t iteration)
{
    TcpSocket socket;
    RingBuffer ring(4096);

    WsReaderSetting reader_setting;
    WsReaderImpl<TcpSocket> reader(ring, reader_setting, socket, true, false);
    WsReaderImpl<TcpSocket> moved_reader(std::move(reader));

    const std::string payload(64 + (iteration % 17), 'x');
    WsWriterImpl<TcpSocket> writer(WsWriterSetting::byServer(), socket);
    auto send_operation = writer.sendText(payload);
    const bool operation_ready = send_operation.await_ready();
    WsWriterImpl<TcpSocket> moved_writer(std::move(writer));

    return !operation_ready &&
           moved_writer.getRemainingBytes() >= payload.size() &&
           moved_writer.getIovecsData() != nullptr &&
           moved_writer.getIovecsCount() > 0;
}

bool exerciseSessionAndUpgraderState(size_t)
{
    TcpSocket socket;
    RingBuffer ring(4096);
    WsUrl url = makeLocalUrl();
    WsReaderSetting reader_setting;
    WsWriterSetting writer_setting = WsWriterSetting::byClient();
    std::unique_ptr<WsConnImpl<TcpSocket>> ws_conn;

    WsSessionImpl<TcpSocket> session(socket, url, writer_setting, 4096, reader_setting);
    WsSessionUpgraderImpl<TcpSocket> session_upgrader = session.upgrade();
    WsSessionUpgraderImpl<TcpSocket> moved_session_upgrader(std::move(session_upgrader));

    WsUpgraderImpl<TcpSocket> client_upgrader(
        &socket,
        &ring,
        url,
        reader_setting,
        writer_setting,
        &ws_conn);
    WsUpgraderImpl<TcpSocket> moved_client_upgrader(std::move(client_upgrader));

    return !session.isUpgraded();
}

} // namespace

int main(int argc, char** argv)
{
    const size_t iterations = parseIterations(argc, argv);

    require(exerciseBuilderCloneMove(0), "builder clone/move fixture should pass");
    require(exerciseReaderWriterMove(0), "reader/writer move fixture should pass");
    require(exerciseSessionAndUpgraderState(0), "session/upgrader fixture should pass");

    runBench("BM_BuilderCloneMove", iterations, exerciseBuilderCloneMove);
    runBench("BM_ReaderWriterMove", iterations, exerciseReaderWriterMove);
    runBench("BM_SessionUpgraderState", iterations, exerciseSessionAndUpgraderState);

    return 0;
}
