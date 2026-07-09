#include <galay/cpp/galay-http2/client/h2c_client.h>
#include <galay/cpp/galay-http2/server/http2_server.h>
#include <galay/cpp/galay-kernel/common/sleep.hpp>
#include <galay/cpp/galay-kernel/core/runtime.h>

#include <cerrno>
#include <charconv>
#include <chrono>
#include <expected>
#include <filesystem>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace galay::http2;
using namespace galay::kernel;

namespace {

struct BenchState {
    std::atomic<bool> done{false};
    size_t requests = 0;
    size_t errors = 0;
    size_t bytes = 0;
    double seconds = 0.0;
};

std::atomic<int64_t> g_fallback_requests{0};

std::expected<size_t, const char*> parseSize(std::string_view text)
{
    size_t value = 0;
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end) {
        return std::unexpected("invalid numeric argument");
    }
    return value;
}

std::expected<void, const char*> writeFilledFile(const std::filesystem::path& path,
                                                 size_t size,
                                                 char fill)
{
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return std::unexpected("open failed");
    }

    std::string chunk(std::min<size_t>(size, 64 * 1024), fill);
    size_t written_total = 0;
    while (written_total < size) {
        const size_t write_size = std::min<size_t>(chunk.size(), size - written_total);
        const ssize_t written = ::write(fd, chunk.data(), write_size);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            const int close_result = ::close(fd);
            if (close_result != 0) {
                return std::unexpected("close after write failure failed");
            }
            return std::unexpected("write failed");
        }
        if (written == 0) {
            const int close_result = ::close(fd);
            if (close_result != 0) {
                return std::unexpected("close after short write failed");
            }
            return std::unexpected("short write");
        }
        written_total += static_cast<size_t>(written);
    }

    const int close_result = ::close(fd);
    if (close_result != 0) {
        return std::unexpected("close failed");
    }
    return {};
}

Task<void> fallbackActiveHandler(Http2ConnContext& ctx)
{
    while (true) {
        auto streams = co_await ctx.getActiveStreams(64);
        if (!streams) {
            break;
        }
        for (auto& stream : *streams) {
            auto events = stream->takeEvents();
            if (!hasHttp2StreamEvent(events, Http2StreamEvent::RequestComplete)) {
                continue;
            }
            g_fallback_requests.fetch_add(1, std::memory_order_relaxed);
            stream->sendHeaders(
                Http2Headers().status(404).contentType("text/plain").contentLength(0),
                true,
                true);
        }
    }
    co_return;
}

Task<void> runClient(uint16_t port,
                     size_t requests,
                     size_t file_size,
                     BenchState* state)
{
    H2cClient<> client(H2cClientBuilder().build());
    auto connect_result = co_await client.connect("127.0.0.1", port);
    if (!connect_result) {
        ++state->errors;
        state->done.store(true, std::memory_order_release);
        co_return;
    }
    auto upgrade_result = co_await client.upgrade("/files/payload.bin");
    if (!upgrade_result) {
        ++state->errors;
        state->done.store(true, std::memory_order_release);
        co_return;
    }

    const auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < requests; ++i) {
        auto stream = client.get("/files/payload.bin");
        if (!stream) {
            ++state->errors;
            continue;
        }
        auto completed = co_await stream->waitResponseComplete();
        if (!completed ||
            stream->response().status != 200 ||
            stream->response().body.size() != file_size) {
            ++state->errors;
            continue;
        }
        if (file_size > 0 && stream->response().body.front() != 'x') {
            ++state->errors;
            continue;
        }
        state->bytes += stream->response().body.size();
    }
    state->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    state->requests = requests;

    auto shutdown_result = co_await client.shutdown();
    if (!shutdown_result.has_value()) {
        ++state->errors;
    }
    state->done.store(true, std::memory_order_release);
    co_return;
}

} // namespace

int main(int argc, char** argv)
{
    size_t requests = 128;
    size_t file_size = 16 * 1024;
    if (argc > 1) {
        auto parsed = parseSize(argv[1]);
        if (!parsed.has_value()) {
            std::cerr << parsed.error() << "\n";
            return 1;
        }
        requests = *parsed;
    }
    if (argc > 2) {
        auto parsed = parseSize(argv[2]);
        if (!parsed.has_value()) {
            std::cerr << parsed.error() << "\n";
            return 1;
        }
        file_size = *parsed;
    }
    if (requests == 0) {
        std::cerr << "requests must be greater than zero\n";
        return 1;
    }

    namespace fs = std::filesystem;
    std::error_code ec;
    const auto base = fs::temp_directory_path(ec) /
        ("galay-h2-static-async-pressure-" + std::to_string(::getpid()));
    if (ec) {
        std::cerr << "temp_directory_path failed\n";
        return 1;
    }
    const auto root = base / "public";
    const bool created = fs::create_directories(root, ec);
    if (ec || !created) {
        std::cerr << "create_directories failed\n";
        return 1;
    }
    auto write_result = writeFilledFile(root / "payload.bin", file_size, 'x');
    if (!write_result.has_value()) {
        std::cerr << write_result.error() << "\n";
        const auto removed = fs::remove_all(base, ec);
        if (ec) {
            std::cerr << "cleanup failed after write failure\n";
        }
        if (removed == 0) {
            std::cerr << "cleanup removed no files after write failure\n";
        }
        return 1;
    }

    const uint16_t port = static_cast<uint16_t>(24000 + (::getpid() % 10000));
    H2cServer server(H2cServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .ioSchedulerCount(1)
        .computeSchedulerCount(0)
        .staticFiles("/files", H2StaticFileConfig{
            .root = root,
            .small_file_threshold = file_size,
        })
        .activeConnHandler(fallbackActiveHandler)
        .build());
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    auto runtime_started = runtime.start();
    if (!runtime_started.has_value()) {
        server.stop();
        const auto removed = fs::remove_all(base, ec);
        if (ec || removed == 0) {
            std::cerr << "cleanup failed after runtime start failure\n";
        }
        std::cerr << "runtime start failed\n";
        return 1;
    }

    BenchState state;
    auto scheduled = runtime.spawn(runClient(port, requests, file_size, &state));
    if (!scheduled.has_value()) {
        runtime.stop();
        server.stop();
        const auto removed = fs::remove_all(base, ec);
        if (ec || removed == 0) {
            std::cerr << "cleanup failed after schedule failure\n";
        }
        std::cerr << "client schedule failed\n";
        return 1;
    }

    for (size_t i = 0; i < 1000 && !state.done.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    runtime.stop();
    server.stop();
    const auto removed = fs::remove_all(base, ec);
    if (ec || removed == 0) {
        std::cerr << "cleanup failed\n";
        return 1;
    }

    const auto fallback = g_fallback_requests.load(std::memory_order_acquire);
    const double rps = state.seconds > 0.0
        ? static_cast<double>(state.requests) / state.seconds
        : 0.0;
    const double mib_per_second = state.seconds > 0.0
        ? static_cast<double>(state.bytes) / (1024.0 * 1024.0) / state.seconds
        : 0.0;

    std::cout << "HTTP/2 static file async pressure\nrequests=" << state.requests
              << "\nfile_size=" << file_size
              << "\nbytes=" << state.bytes
              << "\nseconds=" << state.seconds
              << "\nrps=" << rps
              << "\nMiB_per_second=" << mib_per_second
              << "\nerrors=" << state.errors
              << "\nfallback_requests=" << fallback
              << "\n";

    if (!state.done.load(std::memory_order_acquire) ||
        state.errors != 0 ||
        fallback != 0) {
        return 1;
    }
    return 0;
}
