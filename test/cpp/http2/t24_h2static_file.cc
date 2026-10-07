/**
 * @file t89_h2static_file.cc
 * @brief HTTP/2 static file metadata/cache tests
 */

#include <galay/cpp/galay-http2/server/h2_static_file.h>
#include <galay/cpp/galay-http2/client/h2c_client.h>
#include <galay/cpp/galay-http2/server/http2_server.h>
#include <galay/cpp/galay-kernel/core/runtime.h>

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

std::string header_value(const H2StaticFileLookup& lookup, const std::string& name)
{
    for (const auto& header : lookup.headers) {
        if (header.name == name) {
            return header.value;
        }
    }
    return "";
}

std::string response_header(const Http2Stream::ptr& stream, const std::string& name)
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

Task<void> fallback_active_handler(Http2ConnContext& ctx)
{
    while (true) {
        auto streams = co_await ctx.get_active_streams(16);
        if (!streams) {
            break;
        }
        for (auto& stream : *streams) {
            auto events = stream->take_events();
            if (!has_http2_stream_event(events, Http2StreamEvent::RequestComplete)) {
                continue;
            }
            g_fallback_requests.fetch_add(1, std::memory_order_relaxed);
            stream->send_headers(
                Http2Headers().status(500).content_type("text/plain").content_length(0),
                true,
                true);
        }
    }
    co_return;
}

Http2Stream::ptr send_request(H2cClient<>& client,
                             uint16_t port,
                             const std::string& method,
                             const std::string& path,
                             std::vector<Http2HeaderField> extra = {})
{
    auto* conn = client.get_conn();
    if (conn == nullptr || conn->stream_manager() == nullptr) {
        return nullptr;
    }

    auto stream = conn->stream_manager()->allocate_stream();
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
    stream->send_headers(headers, true, true);
    return stream;
}

Task<void> run_integration_client(uint16_t port)
{
    H2cClient<> client(H2cClientBuilder().build());
    auto connect_result = co_await client.connect("127.0.0.1", port);
    if (!connect_result) {
        std::cerr << "[T89] connect failed: " << connect_result.error().message() << "\n";
        g_done = true;
        co_return;
    }
    auto upgrade_result = co_await client.upgrade("/files/small.txt");
    if (!upgrade_result) {
        std::cerr << "[T89] upgrade failed: " << upgrade_result.error().to_string() << "\n";
        g_done = true;
        co_return;
    }

    auto small = client.get("/files/small.txt");
    auto small_done = co_await small->wait_response_complete();
    if (!small_done || small->response().status != 200 ||
        small->response().body != std::string(1024, 'a') ||
        response_header(small, "content-length") != "1024" ||
        response_header(small, "content-type") != "text/plain") {
        std::cerr << "[T89] GET 1KB file failed status=" << small->response().status
                  << " size=" << small->response().body.size() << "\n";
        g_done = true;
        co_return;
    }

    auto small_query = client.get("/files/small.txt?fast=1");
    auto small_query_done = co_await small_query->wait_response_complete();
    if (!small_query_done || small_query->response().status != 200 ||
        small_query->response().body != std::string(1024, 'a') ||
        response_header(small_query, "content-length") != "1024") {
        std::cerr << "[T89] GET query file failed status=" << small_query->response().status
                  << " size=" << small_query->response().body.size() << "\n";
        g_done = true;
        co_return;
    }

    auto medium = client.get("/files/medium.bin");
    auto medium_done = co_await medium->wait_response_complete();
    if (!medium_done || medium->response().status != 200 ||
        medium->response().body != std::string(16 * 1024, 'm') ||
        response_header(medium, "content-length") != std::to_string(16 * 1024)) {
        std::cerr << "[T89] GET 16KB file failed status=" << medium->response().status
                  << " size=" << medium->response().body.size() << "\n";
        g_done = true;
        co_return;
    }

    auto head = send_request(client, port, "HEAD", "/files/small.txt");
    auto head_done = co_await head->wait_response_complete();
    if (!head_done || head->response().status != 200 ||
        !head->response().body.empty() ||
        response_header(head, "content-length") != "1024") {
        std::cerr << "[T89] HEAD file failed status=" << head->response().status
                  << " size=" << head->response().body.size() << "\n";
        g_done = true;
        co_return;
    }

    auto range = send_request(
        client, port, "GET", "/files/medium.bin", {{"range", "bytes=0-99"}});
    auto range_done = co_await range->wait_response_complete();
    if (!range_done || range->response().status != 206 ||
        range->response().body != std::string(100, 'm') ||
        response_header(range, "content-range") !=
            ("bytes 0-99/" + std::to_string(16 * 1024))) {
        std::cerr << "[T89] Range file failed status=" << range->response().status
                  << " size=" << range->response().body.size()
                  << " content-range=" << response_header(range, "content-range") << "\n";
        g_done = true;
        co_return;
    }

    auto invalid_range = send_request(
        client, port, "GET", "/files/medium.bin", {{"range", "bytes=999999-1000000"}});
    auto invalid_done = co_await invalid_range->wait_response_complete();
    if (!invalid_done || invalid_range->response().status != 416 ||
        response_header(invalid_range, "content-range") !=
            ("bytes */" + std::to_string(16 * 1024))) {
        std::cerr << "[T89] invalid Range failed status=" << invalid_range->response().status
                  << " content-range=" << response_header(invalid_range, "content-range") << "\n";
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
    if (hit.status != 200 ||
        hit.file_size != 5 ||
        hit.content_type != "text/plain" ||
        hit.body_cached ||
        hit.body != nullptr ||
        !hit.body_cacheable ||
        !hit.body_cache_slot ||
        hit.etag.empty()) {
        std::cerr << "[T89] initial H2 static cache lookup must cache metadata without synchronous body read\n";
        return 1;
    }
    auto published_body = std::make_shared<const std::string>("hello");
    const bool published = hit.body_cache_slot->store_if_empty(published_body);
    if (!published) {
        std::cerr << "[T89] initial H2 static body cache publish failed\n";
        return 1;
    }
    auto hit_again = cache.lookup(H2StaticFileRequest{.path = "/hello.txt"});
    if (!hit.encoded_headers ||
        hit_again.encoded_headers != hit.encoded_headers ||
        !hit_again.body_cached ||
        !hit_again.body ||
        *hit_again.body != "hello" ||
        hit_again.body_cache_slot != hit.body_cache_slot) {
        std::cerr << "[T89] H2 static body cache must be reused after async publish\n";
        return 1;
    }
    auto hit_query_a = cache.lookup(H2StaticFileRequest{.path = "/hello.txt?x=1"});
    auto hit_query_b = cache.lookup(H2StaticFileRequest{.path = "/hello.txt?x=2"});
    if (hit_query_a.status != 200 ||
        hit_query_b.status != 200 ||
        hit_query_a.encoded_headers != hit.encoded_headers ||
        hit_query_b.encoded_headers != hit.encoded_headers ||
        hit_query_a.body != hit_again.body ||
        hit_query_b.body != hit_again.body) {
        std::cerr << "[T89] H2 static query aliases must share cached metadata and body\n";
        return 1;
    }
    auto fast_hit = cache.lookup_fast200("/hello.txt?fast=1");
    if (!fast_hit.has_value() ||
        fast_hit->content_length != 5 ||
        fast_hit->encoded_headers != hit.encoded_headers ||
        fast_hit->body != hit_again.body ||
        header_value(hit, "content-length") != "5" ||
        header_value(hit, "content-type") != "text/plain" ||
        header_value(hit, "etag") != hit.etag) {
        std::cerr << "[T89] H2 static fast lookup must reuse encoded headers and async body cache\n";
        return 1;
    }

    auto not_modified = cache.lookup(H2StaticFileRequest{
        .path = "/hello.txt",
        .if_none_match = hit.etag,
    });
    assert(not_modified.status == 304);
    assert(not_modified.file_size == 5);
    assert(not_modified.body == nullptr);
    assert(header_value(not_modified, "etag") == hit.etag);

    auto missing = cache.lookup(H2StaticFileRequest{.path = "/missing.txt"});
    assert(missing.status == 404);
    assert(missing.body == nullptr);

    auto escaped = cache.lookup(H2StaticFileRequest{.path = "/../secret.txt"});
    assert(escaped.status == 404);
    assert(escaped.body == nullptr);

    H2cServerConfig static_config;
    static_config.static_file_mounts.push_back(
        make_h2_static_file_mount("/files", H2StaticFileConfig{.root = root}));
    Http2RuntimeConfig runtime_a;
    Http2RuntimeConfig runtime_b;
    runtime_a.from(static_config);
    runtime_b.from(static_config);
    assert(runtime_a.static_file_mounts.size() == 1);
    assert(runtime_b.static_file_mounts.size() == 1);
    assert(static_config.static_file_mounts[0].cache != nullptr);
    assert(runtime_a.static_file_mounts[0].cache != nullptr);
    assert(runtime_b.static_file_mounts[0].cache != nullptr);
    if (runtime_a.static_file_mounts[0].cache != static_config.static_file_mounts[0].cache ||
        runtime_b.static_file_mounts[0].cache != static_config.static_file_mounts[0].cache ||
        runtime_a.static_file_mounts[0].cache != runtime_b.static_file_mounts[0].cache) {
        std::cerr << "[T89] H2C runtime configs must share server-level static file cache\n";
        return 1;
    }

#ifdef GALAY_SSL_FEATURE_ENABLED
    H2ServerConfig tls_static_config;
    tls_static_config.static_file_mounts.push_back(
        make_h2_static_file_mount("/files", H2StaticFileConfig{.root = root}));
    Http2RuntimeConfig tls_runtime_a;
    Http2RuntimeConfig tls_runtime_b;
    tls_runtime_a.from(tls_static_config);
    tls_runtime_b.from(tls_static_config);
    assert(tls_runtime_a.static_file_mounts.size() == 1);
    assert(tls_runtime_b.static_file_mounts.size() == 1);
    assert(tls_static_config.static_file_mounts[0].cache != nullptr);
    assert(tls_runtime_a.static_file_mounts[0].cache != nullptr);
    assert(tls_runtime_b.static_file_mounts[0].cache != nullptr);
    if (tls_runtime_a.static_file_mounts[0].cache != tls_static_config.static_file_mounts[0].cache ||
        tls_runtime_b.static_file_mounts[0].cache != tls_static_config.static_file_mounts[0].cache ||
        tls_runtime_a.static_file_mounts[0].cache != tls_runtime_b.static_file_mounts[0].cache) {
        std::cerr << "[T89] H2 TLS runtime configs must share server-level static file cache\n";
        return 1;
    }
#endif

    std::ofstream(root / "small.txt") << std::string(1024, 'a');
    std::ofstream(root / "medium.bin") << std::string(16 * 1024, 'm');

    const uint16_t port = static_cast<uint16_t>(23000 + (::getpid() % 10000));
    H2cServer server(H2cServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .io_scheduler_count(1)
        .parallel_scheduler_count(0)
        .static_files("/files", H2StaticFileConfig{.root = root})
        .active_conn_handler(fallback_active_handler)
        .build());
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    Runtime runtime = RuntimeBuilder().io_scheduler_count(1).parallel_scheduler_count(0).build();
    runtime.start();
    auto* scheduler = runtime.get_next_io_scheduler();
    assert(scheduler != nullptr);
    schedule_task(scheduler, run_integration_client(port));
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
