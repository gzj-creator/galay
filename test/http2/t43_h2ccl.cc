#include <galay/cpp/galay-http2/server/http2_server.h>
#include <galay/cpp/galay-http2/client/h2c_client.h>
#include <galay/cpp/galay-kernel/core/runtime.h>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <unistd.h>

using namespace galay::http2;
using namespace galay::kernel;

enum class ClientStage : int {
    Init = 0,
    Connected,
    Upgraded,
    ResponseReady,
    ShutdownDone,
};

static std::atomic<ClientStage> g_stage{ClientStage::Init};
static std::atomic<bool> g_done{false};
static std::atomic<bool> g_ok{false};

Task<void> handleStream(Http2Stream::ptr stream) {
    auto request_done = co_await stream->waitRequestComplete();
    if (!request_done) {
        co_return;
    }

    auto body = stream->request().takeCoalescedBody();
    stream->sendHeaders(
        Http2Headers().status(200).contentType("text/plain").contentLength(body.size()),
        body.empty(), true);
    if (!body.empty()) {
        stream->sendData(std::move(body), true);
    }
    co_return;
}

Task<void> runClient(uint16_t port) {
    H2cClient client(H2cClientBuilder().build());

    auto connect_result = co_await client.connect("127.0.0.1", port);
    if (!connect_result) {
        g_done = true;
        co_return;
    }
    g_stage = ClientStage::Connected;

    auto upgrade_result = co_await client.upgrade("/shutdown");
    if (!upgrade_result) {
        g_done = true;
        co_return;
    }
    g_stage = ClientStage::Upgraded;

    auto stream = client.post("/shutdown", "ping", "text/plain");
    if (!stream) {
        g_done = true;
        co_return;
    }

    auto response_done = co_await stream->waitResponseComplete();
    if (!response_done ||
        stream->response().status != 200 ||
        stream->response().body != "ping") {
        g_done = true;
        co_return;
    }
    g_stage = ClientStage::ResponseReady;

    auto shutdown_result = co_await client.shutdown();
    if (!shutdown_result) {
        g_done = true;
        co_return;
    }

    g_stage = ClientStage::ShutdownDone;
    g_ok = true;
    g_done = true;
    co_return;
}

int main() {

    const uint16_t port = static_cast<uint16_t>(21000 + (::getpid() % 10000));

    H2cServer server(H2cServerBuilder()
        .host("127.0.0.1")
        .port(port)
        .ioSchedulerCount(1)
        .computeSchedulerCount(0)
        .streamHandler(handleStream)
        .build());
    server.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    Runtime runtime = RuntimeBuilder().ioSchedulerCount(1).computeSchedulerCount(0).build();
    runtime.start();
    auto* scheduler = runtime.getNextIOScheduler();
    if (!scheduler) {
        std::cerr << "[T43] missing IO scheduler\n";
        server.stop();
        return 1;
    }
    scheduleTask(scheduler, runClient(port));

    for (int i = 0; i < 100; ++i) {
        if (g_done.load(std::memory_order_acquire)) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    runtime.stop();
    server.stop();

    if (!g_done.load(std::memory_order_acquire)) {
        std::cerr << "[T43] client timed out at stage "
                  << static_cast<int>(g_stage.load(std::memory_order_acquire)) << "\n";
        return 1;
    }
    if (!g_ok.load(std::memory_order_acquire)) {
        std::cerr << "[T43] client shutdown did not complete successfully, stage "
                  << static_cast<int>(g_stage.load(std::memory_order_acquire)) << "\n";
        return 1;
    }

    std::cout << "T43-H2cClientShutdown PASS\n";
    return 0;
}
