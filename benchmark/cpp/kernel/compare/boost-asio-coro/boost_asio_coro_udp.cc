/**
 * @file boost_asio_coro_udp.cc
 * @brief Boost.Asio coroutine UDP echo baseline for the kernel benchmark.
 *
 * The workload matches b6_udp_socket_throughput: loopback IPv4, 100 client
 * coroutines, four SO_REUSEPORT server worker coroutines, 256-byte messages,
 * a one-second warmup, and a five-second measured window. Each client keeps
 * one request in flight to prevent a client-side UDP burst from hiding the
 * request/response event-loop cost. Both sides use unconnected UDP
 * send-to/receive-from operations so the baseline measures the same datagram
 * API shape as Galay's AsyncUdpSocket benchmark.
 */

#include <utility>
#include <boost/asio.hpp>
#include <boost/asio/experimental/parallel_group.hpp>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/socket.h>
#endif

namespace asio = boost::asio;
using asio::awaitable;
using asio::co_spawn;
using asio::detached;
using asio::redirect_error;
using asio::use_awaitable;
using asio::ip::udp;
namespace asio_experimental = boost::asio::experimental;

namespace {

constexpr std::size_t kDefaultClients = 100;
constexpr std::size_t kDefaultWorkers = 4;
constexpr std::size_t kDefaultPayloadBytes = 256;
constexpr std::chrono::seconds kDefaultWarmup{1};
constexpr std::chrono::seconds kDefaultDuration{5};
constexpr std::chrono::milliseconds kDrainWindow{250};
constexpr std::chrono::milliseconds kServerReceiveTimeout{100};
constexpr std::size_t kPipeline = 1;
constexpr std::size_t kMaxPayloadBytes = 65'000;
constexpr std::uint16_t kServerPort = 9090;
constexpr char kWarmupMarker = 'W';
constexpr char kMeasuredMarker = 'M';
constexpr int kServerReceiveBufferBytes = 8 * 1024 * 1024;
constexpr int kClientSendBufferBytes = 2 * 1024 * 1024;

enum class Phase : std::uint8_t { warmup, measured, drain, stopped };

struct Config {
    std::size_t clients = kDefaultClients;
    std::size_t workers = kDefaultWorkers;
    std::size_t payload_bytes = kDefaultPayloadBytes;
    std::chrono::seconds warmup = kDefaultWarmup;
    std::chrono::seconds duration = kDefaultDuration;
};

struct Counters {
    std::atomic<std::uint64_t> client_sent{0};
    std::atomic<std::uint64_t> client_received{0};
    std::atomic<std::uint64_t> client_bytes_sent{0};
    std::atomic<std::uint64_t> client_bytes_received{0};
    std::atomic<std::uint64_t> server_received{0};
    std::atomic<std::uint64_t> server_sent{0};
    std::atomic<std::uint64_t> server_bytes_received{0};
    std::atomic<std::uint64_t> server_bytes_sent{0};
    std::atomic<std::uint64_t> runtime_errors{0};
    std::atomic<std::uint64_t> shutdown_errors{0};
    std::atomic<std::size_t> clients_ready{0};
    std::atomic<std::size_t> clients_failed{0};
    std::atomic<std::size_t> clients_done{0};
    std::atomic<std::size_t> servers_done{0};
};

struct Snapshot {
    std::uint64_t client_sent = 0;
    std::uint64_t client_received = 0;
    std::uint64_t client_bytes_sent = 0;
    std::uint64_t client_bytes_received = 0;
    std::uint64_t server_received = 0;
    std::uint64_t server_sent = 0;
    std::uint64_t server_bytes_received = 0;
    std::uint64_t server_bytes_sent = 0;
    std::uint64_t runtime_errors = 0;
    std::uint64_t shutdown_errors = 0;
};

struct ReceiveResult {
    std::size_t bytes = 0;
    boost::system::error_code error;
    bool timed_out = false;
    udp::endpoint peer;
};

struct BenchmarkState {
    asio::io_context& context;
    const Config& config;
    Counters counters;
    std::atomic<Phase> phase{Phase::warmup};
    std::vector<udp::socket> server_sockets;
    udp::endpoint endpoint;
    std::chrono::steady_clock::time_point measured_started{};
    std::chrono::steady_clock::time_point measured_ended{};
    Snapshot measured;
    Snapshot settled;
};

bool parsePositive(const char* text, std::size_t& value) noexcept
{
    if (text == nullptr || *text == '\0') return false;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0' || parsed == 0) return false;
    value = static_cast<std::size_t>(parsed);
    return true;
}

bool parseSeconds(const char* text, std::chrono::seconds& value) noexcept
{
    std::size_t parsed = 0;
    if (!parsePositive(text, parsed)) return false;
    value = std::chrono::seconds(parsed);
    return true;
}

bool parseConfig(int argc, char** argv, Config& config) noexcept
{
    for (int index = 1; index < argc; ++index) {
        const std::string_view option(argv[index]);
        if (option == "--clients" || option == "--workers" || option == "--size" ||
            option == "--warmup" || option == "--duration") {
            if (index + 1 >= argc) return false;
            const char* value = argv[++index];
            if (option == "--clients") {
                if (!parsePositive(value, config.clients)) return false;
            } else if (option == "--workers") {
                if (!parsePositive(value, config.workers)) return false;
            } else if (option == "--size") {
                if (!parsePositive(value, config.payload_bytes) ||
                    config.payload_bytes > kMaxPayloadBytes) return false;
            } else if (option == "--warmup") {
                if (!parseSeconds(value, config.warmup)) return false;
            } else if (option == "--duration") {
                if (!parseSeconds(value, config.duration)) return false;
            }
        } else {
            return false;
        }
    }
    return config.clients > 0 && config.workers > 0 && config.payload_bytes > 0;
}

void printUsage(const char* program)
{
    std::cout << "Usage: " << program
              << " [--clients N] [--workers N] [--size BYTES]"
                 " [--warmup SECONDS] [--duration SECONDS]\n";
}

bool countTraffic(Phase phase) noexcept
{
    return phase == Phase::measured || phase == Phase::drain;
}

void addCounter(std::atomic<std::uint64_t>& counter, std::uint64_t value = 1) noexcept
{
    const std::uint64_t previous = counter.fetch_add(value, std::memory_order_relaxed);
    if (previous > std::numeric_limits<std::uint64_t>::max() - value) {
        counter.store(std::numeric_limits<std::uint64_t>::max(), std::memory_order_relaxed);
    }
}

void recordError(BenchmarkState& state,
                 const boost::system::error_code& error) noexcept
{
    if (!error) return;
    if (error == asio::error::operation_aborted) return;
    const Phase current = state.phase.load(std::memory_order_acquire);
    if (current >= Phase::drain) {
        state.counters.shutdown_errors.fetch_add(1, std::memory_order_relaxed);
    } else {
        state.counters.runtime_errors.fetch_add(1, std::memory_order_relaxed);
    }
}

Snapshot snapshot(const BenchmarkState& state) noexcept
{
    return Snapshot{
        .client_sent = state.counters.client_sent.load(std::memory_order_relaxed),
        .client_received = state.counters.client_received.load(std::memory_order_relaxed),
        .client_bytes_sent = state.counters.client_bytes_sent.load(std::memory_order_relaxed),
        .client_bytes_received = state.counters.client_bytes_received.load(std::memory_order_relaxed),
        .server_received = state.counters.server_received.load(std::memory_order_relaxed),
        .server_sent = state.counters.server_sent.load(std::memory_order_relaxed),
        .server_bytes_received = state.counters.server_bytes_received.load(std::memory_order_relaxed),
        .server_bytes_sent = state.counters.server_bytes_sent.load(std::memory_order_relaxed),
        .runtime_errors = state.counters.runtime_errors.load(std::memory_order_relaxed),
        .shutdown_errors = state.counters.shutdown_errors.load(std::memory_order_relaxed),
    };
}

// A measured-window snapshot can end with replies in flight.  Only the
// post-drain snapshot is eligible for the success gate, and all four packet
// counters plus byte counters must reconcile there.
bool settledCountersMatch(const Snapshot& values) noexcept
{
    return values.client_sent == values.client_received &&
           values.client_received == values.server_received &&
           values.server_received == values.server_sent &&
           values.client_bytes_sent == values.client_bytes_received &&
           values.client_bytes_received == values.server_bytes_received &&
           values.server_bytes_received == values.server_bytes_sent;
}

void resetMeasurementCounters(BenchmarkState& state) noexcept
{
    state.counters.client_sent.store(0, std::memory_order_relaxed);
    state.counters.client_received.store(0, std::memory_order_relaxed);
    state.counters.client_bytes_sent.store(0, std::memory_order_relaxed);
    state.counters.client_bytes_received.store(0, std::memory_order_relaxed);
    state.counters.server_received.store(0, std::memory_order_relaxed);
    state.counters.server_sent.store(0, std::memory_order_relaxed);
    state.counters.server_bytes_received.store(0, std::memory_order_relaxed);
    state.counters.server_bytes_sent.store(0, std::memory_order_relaxed);
    state.counters.runtime_errors.store(0, std::memory_order_relaxed);
    state.counters.shutdown_errors.store(0, std::memory_order_relaxed);
}

bool enableReusePort(udp::socket& socket) noexcept
{
#if defined(SO_REUSEPORT)
    int enabled = 1;
    return ::setsockopt(socket.native_handle(), SOL_SOCKET, SO_REUSEPORT,
                        &enabled, sizeof(enabled)) == 0;
#else
    (void)socket;
    return false;
#endif
}

bool setSocketBuffer(udp::socket& socket, int option, int bytes) noexcept
{
#if defined(SO_RCVBUF) && defined(SO_SNDBUF)
    return ::setsockopt(socket.native_handle(), SOL_SOCKET, option,
                        &bytes, sizeof(bytes)) == 0;
#else
    (void)socket;
    (void)option;
    (void)bytes;
    return true;
#endif
}

awaitable<ReceiveResult> receiveWithTimeout(
    udp::socket& socket,
    asio::mutable_buffer buffer,
    std::chrono::milliseconds timeout)
{
    asio::steady_timer timer(co_await asio::this_coro::executor);
    timer.expires_after(timeout);
    udp::endpoint peer;
    boost::system::error_code receive_error;
    boost::system::error_code timer_error;
    const auto [order, received] = co_await asio_experimental::make_parallel_group(
        [&socket, buffer, &peer, &receive_error](auto token) {
            return socket.async_receive_from(buffer, peer,
                                             redirect_error(token, receive_error));
        },
        [&timer, &timer_error](auto token) {
            return timer.async_wait(redirect_error(token, timer_error));
        }).async_wait(
            asio_experimental::wait_for_one(),
            use_awaitable);

    co_return ReceiveResult{
        .bytes = received,
        .error = receive_error,
        .timed_out = order[0] == 1 && !timer_error,
        .peer = peer,
    };
}

void stopSockets(BenchmarkState& state) noexcept
{
    for (auto& socket : state.server_sockets) {
        boost::system::error_code close_error;
        socket.close(close_error);
        recordError(state, close_error);
    }
}

void finishClient(BenchmarkState& state) noexcept
{
    state.counters.clients_done.fetch_add(1, std::memory_order_release);
}

void finishServer(BenchmarkState& state) noexcept
{
    state.counters.servers_done.fetch_add(1, std::memory_order_release);
}

awaitable<void> serverWorker(BenchmarkState& state,
                             udp::socket& socket)
{
    std::vector<char> buffer(state.config.payload_bytes);
    for (;;) {
        const ReceiveResult result = co_await receiveWithTimeout(
            socket, asio::buffer(buffer.data(), state.config.payload_bytes),
            kServerReceiveTimeout);
        if (result.timed_out) {
            continue;
        }
        if (result.error) {
            recordError(state, result.error);
            break;
        }
        const std::size_t received = result.bytes;
        if (received != state.config.payload_bytes) {
            state.counters.runtime_errors.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        const bool measured_packet = buffer[0] == kMeasuredMarker;
        if (measured_packet && countTraffic(state.phase.load(std::memory_order_acquire))) {
            addCounter(state.counters.server_received);
            addCounter(state.counters.server_bytes_received, received);
        }

        boost::system::error_code send_error;
        const std::size_t sent = co_await socket.async_send_to(
            asio::buffer(buffer.data(), received), result.peer,
            redirect_error(use_awaitable, send_error));
        if (send_error || sent != received) {
            if (send_error) recordError(state, send_error);
            else state.counters.runtime_errors.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (measured_packet && countTraffic(state.phase.load(std::memory_order_acquire))) {
            addCounter(state.counters.server_sent);
            addCounter(state.counters.server_bytes_sent, sent);
        }
    }
    finishServer(state);
    co_return;
}

awaitable<void> client(BenchmarkState& state, std::size_t client_id)
{
    udp::socket socket(state.context);
    boost::system::error_code error;
    socket.open(udp::v4(), error);
    if (error) {
        recordError(state, error);
        state.counters.clients_failed.fetch_add(1, std::memory_order_release);
        finishClient(state);
        co_return;
    }
    if (!setSocketBuffer(socket, SO_SNDBUF, kClientSendBufferBytes)) {
        state.counters.runtime_errors.fetch_add(1, std::memory_order_relaxed);
        state.counters.clients_failed.fetch_add(1, std::memory_order_release);
        boost::system::error_code close_error;
        socket.close(close_error);
        recordError(state, close_error);
        finishClient(state);
        co_return;
    }
    std::vector<char> payload(state.config.payload_bytes);
    std::vector<char> response(state.config.payload_bytes);
    for (std::size_t index = 0; index < state.config.payload_bytes; ++index) {
        payload[index] = static_cast<char>('a' + ((client_id + index) % 26));
    }

    state.counters.clients_ready.fetch_add(1, std::memory_order_release);
    std::size_t measured_sent = 0;
    std::size_t measured_received = 0;
    bool failed = false;
    while (state.phase.load(std::memory_order_acquire) != Phase::stopped) {
        const Phase phase = state.phase.load(std::memory_order_acquire);
        if (phase == Phase::drain) {
            if (measured_received >= measured_sent) break;
            const ReceiveResult result = co_await receiveWithTimeout(
                socket, asio::buffer(response.data(), state.config.payload_bytes),
                std::chrono::milliseconds(50));
            if (result.timed_out) continue;
            if (result.error) {
                recordError(state, result.error);
                continue;
            }
            if (result.bytes == state.config.payload_bytes &&
                response[0] == kMeasuredMarker &&
                countTraffic(state.phase.load(std::memory_order_acquire))) {
                ++measured_received;
                addCounter(state.counters.client_received);
                addCounter(state.counters.client_bytes_received, result.bytes);
            }
            continue;
        }
        for (std::size_t index = 0; index < kPipeline; ++index) {
            if (state.phase.load(std::memory_order_acquire) >= Phase::drain) break;
            payload[0] = phase == Phase::measured ? kMeasuredMarker : kWarmupMarker;
            boost::system::error_code send_error;
            const std::size_t sent = co_await socket.async_send_to(
                asio::buffer(payload.data(), state.config.payload_bytes),
                state.endpoint,
                redirect_error(use_awaitable, send_error));
            if (send_error || sent != state.config.payload_bytes) {
                if (send_error) recordError(state, send_error);
                else state.counters.runtime_errors.fetch_add(1, std::memory_order_relaxed);
                failed = state.phase.load(std::memory_order_acquire) < Phase::drain;
                break;
            }
            const bool measured_send = phase == Phase::measured;
            if (measured_send) {
                ++measured_sent;
                addCounter(state.counters.client_sent);
                addCounter(state.counters.client_bytes_sent, sent);
            }

            const ReceiveResult result = co_await receiveWithTimeout(
                socket, asio::buffer(response.data(), state.config.payload_bytes),
                std::chrono::milliseconds(50));
            // parallel_group cancels the receive when its timer wins. That
            // operation_aborted is the expected representation of a timeout,
            // not a benchmark shutdown error.
            if (result.timed_out) {
                continue;
            }
            if (result.error || result.bytes != state.config.payload_bytes) {
                if (result.error) recordError(state, result.error);
                else state.counters.runtime_errors.fetch_add(1, std::memory_order_relaxed);
                failed = !result.timed_out &&
                         state.phase.load(std::memory_order_acquire) < Phase::drain;
                break;
            }
            if (measured_send && response[0] == kMeasuredMarker &&
                countTraffic(state.phase.load(std::memory_order_acquire))) {
                ++measured_received;
                addCounter(state.counters.client_received);
                addCounter(state.counters.client_bytes_received, result.bytes);
            }
        }
        if (failed) break;
    }

    if (failed) {
        state.counters.clients_failed.fetch_add(1, std::memory_order_release);
    }
    boost::system::error_code close_error;
    socket.close(close_error);
    recordError(state, close_error);
    finishClient(state);
    co_return;
}

awaitable<bool> waitForClientsReady(BenchmarkState& state)
{
    asio::steady_timer timer(state.context);
    for (;;) {
        const std::size_t ready = state.counters.clients_ready.load(std::memory_order_acquire);
        const std::size_t failed = state.counters.clients_failed.load(std::memory_order_acquire);
        if (ready + failed == state.config.clients) co_return failed == 0;
        timer.expires_after(std::chrono::milliseconds(1));
        boost::system::error_code timer_error;
        co_await timer.async_wait(redirect_error(use_awaitable, timer_error));
        if (timer_error) co_return false;
    }
}

awaitable<void> waitForCompletion(BenchmarkState& state)
{
    asio::steady_timer timer(state.context);
    for (;;) {
        const bool clients_done =
            state.counters.clients_done.load(std::memory_order_acquire) == state.config.clients;
        const bool servers_done =
            state.counters.servers_done.load(std::memory_order_acquire) == state.config.workers;
        if (clients_done && servers_done) co_return;
        timer.expires_after(std::chrono::milliseconds(1));
        boost::system::error_code timer_error;
        co_await timer.async_wait(redirect_error(use_awaitable, timer_error));
        if (timer_error) co_return;
    }
}

awaitable<void> controller(BenchmarkState& state)
{
    for (std::size_t index = 0; index < state.config.clients; ++index) {
        co_spawn(state.context, client(state, index), detached);
    }

    if (!co_await waitForClientsReady(state)) {
        state.phase.store(Phase::stopped, std::memory_order_release);
        stopSockets(state);
        co_await waitForCompletion(state);
        co_return;
    }

    asio::steady_timer timer(state.context);
    timer.expires_after(state.config.warmup);
    boost::system::error_code timer_error;
    co_await timer.async_wait(redirect_error(use_awaitable, timer_error));
    if (timer_error) co_return;

    if (state.counters.clients_done.load(std::memory_order_acquire) != 0 ||
        state.counters.servers_done.load(std::memory_order_acquire) != 0) {
        state.phase.store(Phase::stopped, std::memory_order_release);
        stopSockets(state);
        co_await waitForCompletion(state);
        co_return;
    }

    resetMeasurementCounters(state);
    state.phase.store(Phase::measured, std::memory_order_release);
    state.measured_started = std::chrono::steady_clock::now();
    timer.expires_after(state.config.duration);
    co_await timer.async_wait(redirect_error(use_awaitable, timer_error));
    if (timer_error) co_return;

    state.measured = snapshot(state);
    state.measured_ended = std::chrono::steady_clock::now();
    state.phase.store(Phase::drain, std::memory_order_release);
    timer.expires_after(kDrainWindow);
    co_await timer.async_wait(redirect_error(use_awaitable, timer_error));

    state.phase.store(Phase::stopped, std::memory_order_release);
    stopSockets(state);
    co_await waitForCompletion(state);
    state.settled = snapshot(state);
    co_return;
}

bool prepareServers(BenchmarkState& state)
{
    boost::system::error_code error;
    const auto loopback_address = asio::ip::make_address("127.0.0.1", error);
    if (error) {
        std::cerr << "make_address: " << error.message() << '\n';
        return false;
    }
    state.server_sockets.reserve(state.config.workers);
    for (std::size_t index = 0; index < state.config.workers; ++index) {
        state.server_sockets.emplace_back(state.context);
        auto& socket = state.server_sockets.back();
        socket.open(udp::v4(), error);
        if (error) {
            std::cerr << "open: " << error.message() << '\n';
            return false;
        }
        socket.set_option(udp::socket::reuse_address(true), error);
        if (error || !setSocketBuffer(socket, SO_RCVBUF, kServerReceiveBufferBytes)) {
            std::cerr << "server socket options failed\n";
            return false;
        }
        if (!enableReusePort(socket)) {
            std::cerr << "reuse_port: errno=" << errno << '\n';
            return false;
        }
        const udp::endpoint bind_endpoint(
            loopback_address, kServerPort);
        socket.bind(bind_endpoint, error);
        if (error) {
            std::cerr << "bind: " << error.message() << '\n';
            return false;
        }
        if (index == 0) {
            state.endpoint = socket.local_endpoint(error);
            if (error) {
                std::cerr << "local_endpoint: " << error.message() << '\n';
                return false;
            }
        }
    }
    return state.server_sockets.size() == state.config.workers;
}

double rate(std::uint64_t packets, std::chrono::steady_clock::duration duration)
{
    const double seconds = std::chrono::duration<double>(duration).count();
    return seconds > 0.0 ? static_cast<double>(packets) / seconds : 0.0;
}

double lossPercent(const Snapshot& values) noexcept
{
    if (values.client_sent == 0) return 100.0;
    const double loss = (1.0 - static_cast<double>(values.client_received) /
                                  static_cast<double>(values.client_sent)) * 100.0;
    return loss > 0.0 ? loss : 0.0;
}

std::string boostVersion()
{
    const unsigned version = BOOST_VERSION;
    return std::to_string(version / 100000) + "." +
           std::to_string((version / 100) % 1000) + "." +
           std::to_string(version % 100);
}

} // namespace

int main(int argc, char** argv)
{
    Config config;
    if (!parseConfig(argc, argv, config)) {
        printUsage(argv[0]);
        return 2;
    }

    asio::io_context context;
    BenchmarkState state{context, config};
    if (!prepareServers(state)) {
        std::cerr << "boost_asio_coro_udp: failed to prepare SO_REUSEPORT sockets\n";
        return 1;
    }
    for (auto& socket : state.server_sockets) {
        co_spawn(context, serverWorker(state, socket), detached);
    }
    co_spawn(context, controller(state), detached);

    const auto started = std::chrono::steady_clock::now();
    context.run();
    const auto elapsed = std::chrono::steady_clock::now() - started;

    const auto measured_duration = state.measured_ended > state.measured_started
        ? state.measured_ended - state.measured_started
        : std::chrono::duration_cast<std::chrono::steady_clock::duration>(config.duration);
    const double measured_client_rate = rate(state.measured.client_sent, measured_duration);
    const double measured_server_rate = rate(state.measured.server_received, measured_duration);
    const bool ready = state.counters.clients_ready.load(std::memory_order_acquire) == config.clients;
    const bool complete = state.counters.clients_done.load(std::memory_order_acquire) == config.clients &&
                          state.counters.servers_done.load(std::memory_order_acquire) == config.workers;
    const bool measured_ok = ready && complete && state.measured.client_sent > 0 &&
                             state.measured.runtime_errors == 0 &&
                             state.settled.runtime_errors == 0 &&
                             state.settled.shutdown_errors == 0 &&
                             settledCountersMatch(state.settled);

    std::cout << "meta implementation=boost.asio version=" << boostVersion()
              << " coroutine=co_spawn/awaitable scenario=udp-echo"
              << " backend=single-io-context"
              << " clients=" << config.clients
              << " workers=" << config.workers
              << " payload_bytes=" << config.payload_bytes
              << " pipeline=" << kPipeline
              << " warmup_s=" << config.warmup.count()
              << " duration_s=" << config.duration.count()
              << " ready_clients="
              << state.counters.clients_ready.load(std::memory_order_acquire)
              << '\n';
    std::cout << "measured client_sent=" << state.measured.client_sent
              << " client_received=" << state.measured.client_received
              << " client_bytes_sent=" << state.measured.client_bytes_sent
              << " client_bytes_received=" << state.measured.client_bytes_received
              << " server_received=" << state.measured.server_received
              << " server_sent=" << state.measured.server_sent
              << " server_bytes_received=" << state.measured.server_bytes_received
              << " server_bytes_sent=" << state.measured.server_bytes_sent
              << " measurement_ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(measured_duration).count()
              << " client_pkt_s=" << measured_client_rate
              << " server_pkt_s=" << measured_server_rate
              << " client_loss_pct=" << lossPercent(state.measured)
              << " runtime_errors=" << state.measured.runtime_errors
              << " shutdown_errors=" << state.measured.shutdown_errors << '\n';
    std::cout << "settled client_sent=" << state.settled.client_sent
              << " client_received=" << state.settled.client_received
              << " client_bytes_sent=" << state.settled.client_bytes_sent
              << " client_bytes_received=" << state.settled.client_bytes_received
              << " server_received=" << state.settled.server_received
              << " server_sent=" << state.settled.server_sent
              << " server_bytes_received=" << state.settled.server_bytes_received
              << " server_bytes_sent=" << state.settled.server_bytes_sent
              << " elapsed_ms="
              << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
              << " client_loss_pct=" << lossPercent(state.settled)
              << " settled_loss_pct=" << lossPercent(state.settled)
              << " runtime_errors=" << state.settled.runtime_errors
              << " shutdown_errors=" << state.settled.shutdown_errors << '\n';
    std::cout << "status=" << (measured_ok ? "ok" : "fail") << '\n';
    return measured_ok ? 0 : 1;
}
