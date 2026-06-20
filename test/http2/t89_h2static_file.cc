/**
 * @file t89_h2static_file.cc
 * @brief HTTP/2 static file metadata/cache tests
 */

#include "galay-http2/server/h2_static_file.h"
#include "galay-http2/client/h2c_client.h"
#include "galay-http2/server/http2_server.h"
#include "galay-kernel/core/runtime.h"

#include <atomic>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>

using namespace galay::http2;
using namespace galay::kernel;

namespace {

std::string headerValue(const H2StaticFileLookup& lookup, const std::string& name)
{
    for (const auto& header : lookup.headers) {
        if (header.name == name) {
            return header.value;
        }
    }
    return "";
}

std::string responseHeader(const Http2Stream::ptr& stream, const std::string& name)
{
    if (!stream) {
        return "";
    }
    for (const auto& header : stream->response().headers) {
        if (header.name == name) {
            return header.value;
        }
    }
    return "";
}

std::atomic<bool> g_done{false};
std::atomic<bool> g_ok{false};
std::atomic<int> g_fallback_requests{0};

Task<void> fallbackActiveHandler(Http2ConnContext& ctx)
{
    while (true) {
        auto streams = co_await ctx.getActiveStreams(16);
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
                Http2Headers().status(500).contentType("text/plain").contentLength(0),
                true,
                true);
        }
    }
    co_return;
}

Http2Stream::ptr sendRequest(H2cClient& client,
                             uint16_t port,
                             const std::string& method,
                             const std::string& path,
                             std::vector<Http2HeaderField> extra = {})
{
    auto* conn = client.getConn();
    if (conn == nullptr || conn->streamManager() == nullptr) {
        return nullptr;
    }

    auto stream = conn->streamManager()->allocateStream();
    const std::string authority = "127.0.0.1:" + std::to_string(port);
    std::vector<Http2HeaderField> headers{
        {":method", method},
        {":scheme", "http"},
        {":authority", authority},
        {":path", path},
    };
    headers.insert(headers.end(),
                   std::make_move_iterator(extra.begin()),
                   std::make_move_iterator(extra.end()));
    stream->sendHeaders(headers, true, true);
    return stream;
}

Task<void> runIntegrationClient(uint16_t port)
{
    H2cClient client(H2cClientBuilder().build());
    auto connect_result = co_await client.connect("127.0.0.1", port);
    if (!connect_result) {
        std::cerr << "[T89] connect failed: " << connect_result.error().message() << "\n";
        g_done = true;
        co_return;
    }
    auto upgrade_result = co_await client.upgrade("/files/small.txt");
    if (!upgrade_result) {
        std::cerr << "[T89] upgrade failed: " << upgrade_result.error().toString() << "\n";
        g_done = true;
        co_return;
    }

    auto small = client.get("/files/small.txt");
    auto small_done = co_await small->waitResponseComplete();
    if (!small_done || small->response().status != 200 ||
        small->response().body != std::string(1024, 'a') ||
        responseHeader(small, "content-length") != "1024" ||
        responseHeader(small, "content-type") != "text/plain") {
        std::cerr << "[T89] GET 1KB file failed status=" << small->response().status
                  << " size=" << small->response().body.size() << "\n";
        g_done = true;
        co_return;
    }

    auto medium = client.get("/files/medium.bin");
    auto medium_done = co_await medium->waitResponseComplete();
    if (!medium_done || medium->response().status != 200 ||
        medium->response().body != std::string(16 * 1024, 'm') ||
        responseHeader(medium, "content-length") != std::to_string(16 * 1024)) {
        std::cerr << "[T89] GET 16KB file failed status=" << medium->response().status
                  << " size=" << medium->response().body.size() << "\n";
        g_done = true;
        co_return;
    }

    auto head = sendRequest(client, port, "HEAD", "/files/small.txt");
    auto head_done = co_await head->waitResponseComplete();
    if (!head_done || head->response().status != 200 ||
        !head->response().body.empty() ||
        responseHeader(head, "content-length") != "1024") {
        std::cerr << "[T89] HEAD file failed status=" << head->response().status
                  << " size=" << head->response().body.size() << "\n";
        g_done = true;
        co_return;
    }

    auto range = sendRequest(
        client, port, "GET", "/files/medium.bin", {{"range", "bytes=0-99"}});
    auto range_done = co_await range->waitResponseComplete();
    if (!range_done || range->response().status != 206 ||
        range->response().body != std::string(100, 'm') ||
        responseHeader(range, "content-range") !=
            ("bytes 0-99/" + std::to_string(16 * 1024))) {
        std::cerr << "[T89] Range file failed status=" << range->response().status
                  << " size=" << range->response().body.size()
                  << " content-range=" << responseHeader(range, "content-range") << "\n";
        g_done = true;
        co_return;
    }

    auto invalid_range = sendRequest(
        client, port, "GET", "/files/medium.bin", {{"range", "bytes=999999-1000000"}});
    auto invalid_done = co_await invalid_range->waitResponseComplete();
    if (!invalid_done || invalid_range->response().status != 416 ||
        responseHeader(invalid_range, "content-range") !=
            ("bytes */" + std::to_string(16 * 1024))) {
        std::cerr << "[T89] invalid Range failed status=" << invalid_range->response().status
                  << " content-range=" << responseHeader(invalid_range, "content-range") << "\n";
        g_done = true;
        co_return;
    }

    co_await client.shutdown();
    g_ok = true;
    g_done = true;
    co_return;
}

} // namespace

int main()
{
    namespace fs = std::filesystem;

    const auto base = fs::temp_directory_path() /
        ("galay-h2-static-file-" + std::to_string(::getpid()));
    const auto root = base / "public";
    fs::create_directories(root);

    {
        std::ofstream(root / "hello.txt") << "hello";
        std::ofstream(base / "secret.txt") << "secret";
    }

    H2StaticFileCache cache(H2StaticFileConfig{
        .root = root,
        .small_file_threshold = 64 * 1024,
    });

    auto hit = cache.lookup(H2StaticFileRequest{.path = "/hello.txt"});
    assert(hit.status == 200);
    assert(hit.file_size == 5);
    assert(hit.content_type == "text/plain");
    assert(hit.body_cached);
    assert(hit.body && *hit.body == "hello");
    assert(!hit.etag.empty());
    auto hit_again = cache.lookup(H2StaticFileRequest{.path = "/hello.txt"});
    assert(hit.encoded_headers != nullptr);
    assert(hit_again.encoded_headers == hit.encoded_headers);
    auto hit_query_a = cache.lookup(H2StaticFileRequest{.path = "/hello.txt?x=1"});
    auto hit_query_b = cache.lookup(H2StaticFileRequest{.path = "/hello.txt?x=2"});
    assert(hit_query_a.status == 200);
    assert(hit_query_b.status == 200);
    assert(hit_query_a.encoded_headers == hit.encoded_headers);
    assert(hit_query_b.encoded_headers == hit.encoded_headers);
    assert(hit_query_a.body == hit.body);
    assert(hit_query_b.body == hit.body);
    assert(headerValue(hit, "content-length") == "5");
    assert(headerValue(hit, "content-type") == "text/plain");
    assert(headerValue(hit, "etag") == hit.etag);

    auto not_modified = cache.lookup(H2StaticFileRequest{
        .path = "/hello.txt",
        .if_none_match = hit.etag,
    });
    assert(not_modified.status == 304);
    assert(not_modified.file_size == 5);
    assert(not_modified.body == nullptr);
    assert(headerValue(not_modified, "etag") == hit.etag);

    auto missing = cache.lookup(H2StaticFileRequest{.path = "/missing.txt"});
    assert(missing.status == 404);
    assert(missing.body == nullptr);

    auto escaped = cache.lookup(H2StaticFileRequest{.path = "/../secret.txt"});
    assert(escaped.status == 404);
    assert(escaped.body == nullptr);

    H2cServerConfig static_config;
    static_config.static_file_mounts.push_back(
        makeH2StaticFileMount("/files", H2StaticFileConfig{.root = root}));
    Http2RuntimeConfig runtime_a;
    Http2RuntimeConfig runtime_b;
    runtime_a.from(static_config);
    runtime_b.from(static_config);
    assert(runtime_a.static_file_mounts.size() == 1);
    assert(runtime_b.static_file_mounts.size() == 1);
    assert(static_config.static_file_mounts[0].cache != nullptr);
    assert(runtime_a.static_file_mounts[0].cache != nullptr);
    assert(runtime_b.static_file_mounts[0].cache != nullptr);
    assert(runtime_a.static_file_mounts[0].cache != static_config.static_file_mounts[0].cache);
    assert(runtime_b.static_file_mounts[0].cache != static_config.static_file_mounts[0].cache);
    assert(runtime_a.static_file_mounts[0].cache != runtime_b.static_file_mounts[0].cache);

#ifdef GALAY_SSL_FEATURE_ENABLED
    H2ServerConfig tls_static_config;
    tls_static_config.static_file_mounts.push_back(
        makeH2StaticFileMount("/files", H2StaticFileConfig{.root = root}));
    Http2RuntimeConfig tls_runtime_a;
    Http2RuntimeConfig tls_runtime_b;
    tls_runtime_a.from(tls_static_config);
    tls_runtime_b.from(tls_static_config);
    assert(tls_runtime_a.static_file_mounts.size() == 1);
    assert(tls_runtime_b.static_file_mounts.size() == 1);
    assert(tls_static_config.static_file_mounts[0].cache != nullptr);
    assert(tls_runtime_a.static_file_mounts[0].cache != nullptr);
    assert(tls_runtime_b.static_file_mounts[0].cache != nullptr);
    assert(tls_runtime_a.static_file_mounts[0].cache != tls_static_config.static_file_mounts[0].cache);
    assert(tls_runtime_b.static_file_mounts[0].cache != tls_static_config.static_file_mounts[0].cache);
    assert(tls_runtime_a.static_file_mounts[0].cache != tls_runtime_b.static_file_mounts[0].cache);
#endif

    std::ofstream(root / "small.txt") << std::string(1024, 'a');
    std::ofstream(root / "medium.bin") << std::string(16 * 1024, 'm');

    const uint16_t port = static_cast<uint16_t>(23000 + (::getpid() % 10000));
    H2cServer server(H2cServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .ioSchedulerCount(1)
        .computeSchedulerCount(0)
        .staticFiles("/files", H2StaticFileConfig{.root = root})
        .activeConnHandler(fallbackActiveHandler)
        .build());
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    runtime.start();
    auto* scheduler = runtime.getNextIOScheduler();
    assert(scheduler != nullptr);
    scheduleTask(scheduler, runIntegrationClient(port));
    for (int i = 0; i < 100; ++i) {
        if (g_done.load(std::memory_order_acquire)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    runtime.stop();
    server.stop();

    assert(g_done.load(std::memory_order_acquire));
    assert(g_ok.load(std::memory_order_acquire));
    assert(g_fallback_requests.load(std::memory_order_acquire) == 0);

    fs::remove_all(base);
    std::cout << "t89_h2static_file PASS\n";
    return 0;
}
