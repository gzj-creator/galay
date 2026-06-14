#include "http/server/file_descriptor.h"
#include "http/client/http_client.h"
#include "http/server/http_range.h"
#include "http/protoc/http_chunk.h"
#include "http/protoc/http_header.h"
#include "ws/client/ws_client.h"
#include "kernel/kernel/runtime.h"

#include <atomic>
#include <chrono>
#include <fcntl.h>
#include <iostream>
#include <sys/uio.h>
#include <thread>

using namespace galay::http;
using namespace galay::kernel;
using namespace galay::websocket;

namespace {

struct TestState {
    std::atomic<bool> done{false};
    std::atomic<bool> ok{false};
};

bool check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "[T80] " << message << '\n';
        return false;
    }
    return true;
}

template <typename Result>
bool hasBusinessError(const Result& result)
{
    return result.has_value() && !result.value().has_value();
}

Task<void> runClientErrorPropagationChecks(TestState* state)
{
    bool ok = true;

    try {
        HttpClient http_client;
        auto http_connect = co_await http_client.connect("ftp://127.0.0.1/");
        ok = check(hasBusinessError(http_connect), "invalid HTTP URL should return an error") && ok;

        auto http_session = http_client.getSession();
        ok = check(!http_session, "HTTP getSession before connect should return an error") && ok;

        auto http_socket = http_client.socket();
        ok = check(!http_socket, "HTTP socket before connect should return an error") && ok;

        auto http_close = co_await http_client.close();
        ok = check(hasBusinessError(http_close), "HTTP close before connect should return an error") && ok;

        HttpClient failed_http_client;
        auto failed_http_connect = co_await failed_http_client.connect("http://127.0.0.1:1/");
        ok = check(hasBusinessError(failed_http_connect), "HTTP connect to a closed port should return an error") && ok;

        auto failed_http_session = failed_http_client.getSession();
        ok = check(!failed_http_session, "HTTP getSession after failed connect should return an error") && ok;

        auto failed_http_socket = failed_http_client.socket();
        ok = check(!failed_http_socket, "HTTP socket after failed connect should return an error") && ok;

        WsClient ws_client;
        auto ws_connect = co_await ws_client.connect("http://127.0.0.1/ws");
        ok = check(hasBusinessError(ws_connect), "invalid WebSocket URL should return an error") && ok;

        auto ws_session = ws_client.getSession(WsWriterSetting::byClient());
        ok = check(!ws_session, "WS getSession before connect should return an error") && ok;

        auto ws_close = co_await ws_client.close();
        ok = check(hasBusinessError(ws_close), "WS close before connect should return an error") && ok;

        auto ws_handshake = co_await ws_client.handshake();
        ok = check(hasBusinessError(ws_handshake), "WS handshake before connect should return an error") && ok;

        WsClient failed_ws_client;
        auto failed_ws_connect = co_await failed_ws_client.connect("ws://127.0.0.1:1/ws");
        ok = check(hasBusinessError(failed_ws_connect), "WS connect to a closed port should return an error") && ok;

        auto failed_ws_session = failed_ws_client.getSession(WsWriterSetting::byClient());
        ok = check(!failed_ws_session, "WS getSession after failed connect should return an error") && ok;

        WsUrl invalid_upgrader_url;
        WsUpgrader invalid_upgrader(nullptr,
                                    nullptr,
                                    invalid_upgrader_url,
                                    WsReaderSetting(),
                                    WsWriterSetting::byClient(),
                                    nullptr);
        auto invalid_upgrade = co_await invalid_upgrader();
        ok = check(hasBusinessError(invalid_upgrade), "invalid WsUpgrader should return an error") && ok;

#ifdef GALAY_SSL_FEATURE_ENABLED
        HttpsClient failed_https_client;
        auto failed_https_connect = co_await failed_https_client.connect("https://127.0.0.1:1/");
        ok = check(hasBusinessError(failed_https_connect), "HTTPS connect to a closed port should return an error") && ok;

        auto failed_https_session = failed_https_client.getSession();
        ok = check(!failed_https_session, "HTTPS getSession after failed connect should return an error") && ok;

        WssClient failed_wss_client;
        auto failed_wss_connect = co_await failed_wss_client.connect("wss://127.0.0.1:1/ws");
        ok = check(hasBusinessError(failed_wss_connect), "WSS connect to a closed port should return an error") && ok;

        auto failed_wss_session = failed_wss_client.getSession(WsWriterSetting::byClient());
        ok = check(!failed_wss_session, "WSS getSession after failed connect should return an error") && ok;
#endif
    } catch (const std::exception& ex) {
        std::cerr << "[T80] unexpected exception: " << ex.what() << '\n';
        ok = false;
    } catch (...) {
        std::cerr << "[T80] unexpected non-standard exception\n";
        ok = false;
    }

    state->ok = ok;
    state->done = true;
    co_return;
}

bool testFileDescriptorErrorPropagation()
{
    try {
        FileDescriptor fd;
        auto result = fd.open("/tmp/galay-http-t80-missing-file", O_RDONLY);
        if (!check(!result, "FileDescriptor::open should return an error for missing files")) {
            return false;
        }
        if (!check(!fd.valid(), "FileDescriptor should remain invalid after failed open")) {
            return false;
        }
        return true;
    } catch (const std::exception& ex) {
        std::cerr << "[T80] unexpected FileDescriptor exception: " << ex.what() << '\n';
        return false;
    } catch (...) {
        std::cerr << "[T80] unexpected FileDescriptor non-standard exception\n";
        return false;
    }
}

bool testParserErrorPropagation()
{
    try {
        auto huge_range = HttpRangeParser::parse(
            "bytes=999999999999999999999999999999-",
            1024);
        if (!check(!huge_range.isValid(), "huge Range should be rejected without throwing")) {
            return false;
        }

        auto zero_file_range = HttpRangeParser::parse("bytes=-1", 0);
        if (!check(!zero_file_range.isValid(), "zero-size file Range should be invalid")) {
            return false;
        }

        std::string invalid_chunk = "ffffffffffffffffffffffffffffffff\r\n";
        std::vector<iovec> invalid_chunk_iovecs(1);
        invalid_chunk_iovecs[0].iov_base = invalid_chunk.data();
        invalid_chunk_iovecs[0].iov_len = invalid_chunk.size();
        std::string chunk_output;
        auto chunk_result = Chunk::fromIOVec(invalid_chunk_iovecs, chunk_output);
        if (!check(!chunk_result, "oversized chunk length should return an error")) {
            return false;
        }

        std::string chunk_with_extension = "5;trace=1\r\nHello\r\n";
        std::vector<iovec> extension_iovecs(1);
        extension_iovecs[0].iov_base = chunk_with_extension.data();
        extension_iovecs[0].iov_len = chunk_with_extension.size();
        std::string extension_output;
        auto extension_result = Chunk::fromIOVec(extension_iovecs, extension_output);
        if (!check(extension_result && extension_output == "Hello",
                   "valid chunk extension should still parse")) {
            return false;
        }

        HttpResponseHeader invalid_status;
        auto [status_error, consumed] = invalid_status.fromString("HTTP/1.1 200OK\r\n\r\n");
        if (!check(status_error == kHttpCodeInvalid && consumed == -1,
                   "invalid response status should return kHttpCodeInvalid")) {
            return false;
        }

        return true;
    } catch (const std::exception& ex) {
        std::cerr << "[T80] unexpected parser exception: " << ex.what() << '\n';
        return false;
    } catch (...) {
        std::cerr << "[T80] unexpected parser non-standard exception\n";
        return false;
    }
}

} // namespace

int main()
{
    if (!testFileDescriptorErrorPropagation()) {
        return 1;
    }
    if (!testParserErrorPropagation()) {
        return 1;
    }

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    runtime.start();

    auto* scheduler = runtime.getNextIOScheduler();
    if (!scheduler) {
        std::cerr << "[T80] missing IO scheduler\n";
        runtime.stop();
        return 1;
    }

    TestState state;
    scheduleTask(scheduler, runClientErrorPropagationChecks(&state));

    for (int i = 0; i < 100 && !state.done.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    runtime.stop();

    if (!state.done.load()) {
        std::cerr << "[T80] client checks timed out\n";
        return 1;
    }

    return state.ok.load() ? 0 : 1;
}
