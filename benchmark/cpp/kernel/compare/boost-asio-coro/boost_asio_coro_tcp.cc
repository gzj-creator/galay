/**
 * @file boost_asio_coro_tcp.cc
 * @brief Boost.Asio C++ coroutine TCP echo baseline.
 *
 * This is deliberately the same loopback workload as
 * b31_tcp_socket_fair_throughput: 100 long-lived IPv4 connections, four
 * SO_REUSEPORT accept workers, 256-byte framed messages, one request in
 * flight per client, and identical warmup/measurement/drain phases.
 */

#include <boost/asio.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
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
using asio::ip::tcp;

namespace {

constexpr std::size_t kDefaultClients = 100;
constexpr std::size_t kDefaultWorkers = 4;
constexpr std::size_t kDefaultPayloadBytes = 256;
constexpr std::uint16_t kServerPort = 9091;
constexpr std::chrono::seconds kDefaultWarmup{1};
constexpr std::chrono::seconds kDefaultDuration{5};
constexpr std::chrono::milliseconds kDrainWindow{250};
constexpr char kWarmupMarker = 'W';
constexpr char kMeasuredMarker = 'M';
constexpr std::size_t kPipeline = 1;

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
    std::atomic<std::size_t> servers_ready{0};
    std::atomic<std::size_t> servers_failed{0};
    std::atomic<std::size_t> servers_done{0};
    std::atomic<std::size_t> connections_started{0};
    std::atomic<std::size_t> connections_done{0};
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

struct BenchmarkState {
    asio::io_context& context;
    const Config& config;
    Counters counters;
    std::atomic<Phase> phase{Phase::warmup};
    std::vector<std::unique_ptr<tcp::acceptor>> acceptors;
    std::vector<std::unique_ptr<tcp::socket>> client_sockets;
    std::vector<std::unique_ptr<tcp::socket>> server_sockets;
    tcp::endpoint endpoint;
    Snapshot measured;
    Snapshot settled;
    std::chrono::steady_clock::time_point measured_started{};
    std::chrono::steady_clock::time_point measured_ended{};
};

bool parsePositive(const char* text, std::size_t& value) noexcept
{
    if (text == nullptr || *text == '\0') return false;
    char* end = nullptr;
    const auto parsed = std::strtoull(text, &end, 10);
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
        if (option != "--clients" && option != "--workers" && option != "--size" &&
            option != "--warmup" && option != "--duration") {
            return false;
        }
        if (++index >= argc) return false;
        const char* value = argv[index];
        if (option == "--clients") {
            if (!parsePositive(value, config.clients)) return false;
        } else if (option == "--workers") {
            if (!parsePositive(value, config.workers)) return false;
        } else if (option == "--size") {
            if (!parsePositive(value, config.payload_bytes) || config.payload_bytes > 64 * 1024) {
                return false;
            }
        } else if (option == "--warmup") {
            if (!parseSeconds(value, config.warmup)) return false;
        } else if (option == "--duration") {
            if (!parseSeconds(value, config.duration)) return false;
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
    const auto previous = counter.fetch_add(value, std::memory_order_relaxed);
    if (previous > std::numeric_limits<std::uint64_t>::max() - value) {
        counter.store(std::numeric_limits<std::uint64_t>::max(), std::memory_order_relaxed);
    }
}

void recordError(BenchmarkState& state, const boost::system::error_code& error) noexcept
{
    if (!error || error == asio::error::operation_aborted) return;
    if (state.phase.load(std::memory_order_acquire) >= Phase::drain) {
        addCounter(state.counters.shutdown_errors);
    } else {
        addCounter(state.counters.runtime_errors);
    }
}

Snapshot snapshot(const BenchmarkState& state) noexcept
{
    return {
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

bool enableReusePort(tcp::acceptor& acceptor) noexcept
{
#if defined(SO_REUSEPORT)
    int enabled = 1;
    return ::setsockopt(acceptor.native_handle(), SOL_SOCKET, SO_REUSEPORT,
                        &enabled, sizeof(enabled)) == 0;
#else
    (void)acceptor;
    return false;
#endif
}

void closeAllSockets(BenchmarkState& state) noexcept
{
    boost::system::error_code ignored;
    for (auto& acceptor : state.acceptors) {
        acceptor->cancel(ignored);
        acceptor->close(ignored);
    }
    for (auto& socket : state.client_sockets) {
        socket->cancel(ignored);
        socket->close(ignored);
    }
    for (auto& socket : state.server_sockets) {
        socket->cancel(ignored);
        socket->close(ignored);
    }
}

awaitable<bool> readExact(tcp::socket& socket, asio::mutable_buffer buffer)
{
    boost::system::error_code error;
    const std::size_t bytes = co_await asio::async_read(
        socket, buffer, redirect_error(use_awaitable, error));
    co_return !error && bytes == buffer.size();
}

awaitable<bool> writeAll(tcp::socket& socket, asio::const_buffer buffer)
{
    boost::system::error_code error;
    const std::size_t bytes = co_await asio::async_write(
        socket, buffer, redirect_error(use_awaitable, error));
    co_return !error && bytes == buffer.size();
}

awaitable<void> serverConnection(BenchmarkState& state, tcp::socket& socket)
{
    std::vector<char> buffer(state.config.payload_bytes);
    while (state.phase.load(std::memory_order_acquire) != Phase::stopped) {
        const bool read_ok = co_await readExact(
            socket, asio::buffer(buffer.data(), buffer.size()));
        if (!read_ok) break;

        const bool measured_frame = buffer[0] == kMeasuredMarker;
        if (measured_frame && countTraffic(state.phase.load(std::memory_order_acquire))) {
            addCounter(state.counters.server_received);
            addCounter(state.counters.server_bytes_received, buffer.size());
        }

        const bool write_ok = co_await writeAll(
            socket, asio::buffer(buffer.data(), buffer.size()));
        if (!write_ok) break;
        if (measured_frame && countTraffic(state.phase.load(std::memory_order_acquire))) {
            addCounter(state.counters.server_sent);
            addCounter(state.counters.server_bytes_sent, buffer.size());
        }
    }
    boost::system::error_code ignored;
    socket.close(ignored);
    state.counters.connections_done.fetch_add(1, std::memory_order_release);
    co_return;
}

awaitable<void> serverWorker(BenchmarkState& state, tcp::acceptor& acceptor)
{
    state.counters.servers_ready.fetch_add(1, std::memory_order_release);
    for (;;) {
        auto socket = std::make_unique<tcp::socket>(state.context);
        boost::system::error_code error;
        co_await acceptor.async_accept(*socket, redirect_error(use_awaitable, error));
        if (error) {
            recordError(state, error);
            break;
        }
        socket->set_option(tcp::no_delay(true), error);
        if (error) {
            recordError(state, error);
            socket->close(error);
            continue;
        }
        state.server_sockets.push_back(std::move(socket));
        tcp::socket& client = *state.server_sockets.back();
        state.counters.connections_started.fetch_add(1, std::memory_order_release);
        co_spawn(state.context, serverConnection(state, client), detached);
    }
    state.counters.servers_done.fetch_add(1, std::memory_order_release);
    co_return;
}

awaitable<void> client(BenchmarkState& state, std::size_t client_id)
{
    auto socket = std::make_unique<tcp::socket>(state.context);
    state.client_sockets.push_back(std::move(socket));
    tcp::socket& client_socket = *state.client_sockets.back();

    boost::system::error_code error;
    co_await client_socket.async_connect(state.endpoint,
                                         redirect_error(use_awaitable, error));
    if (error) {
        recordError(state, error);
        state.counters.clients_failed.fetch_add(1, std::memory_order_release);
        state.counters.clients_done.fetch_add(1, std::memory_order_release);
        co_return;
    }
    client_socket.set_option(tcp::no_delay(true), error);
    if (error) {
        recordError(state, error);
        state.counters.clients_failed.fetch_add(1, std::memory_order_release);
        state.counters.clients_done.fetch_add(1, std::memory_order_release);
        co_return;
    }

    std::vector<char> payload(state.config.payload_bytes);
    std::vector<char> response(state.config.payload_bytes);
    std::fill(payload.begin(), payload.end(), static_cast<char>('a' + client_id % 26));
    state.counters.clients_ready.fetch_add(1, std::memory_order_release);
    std::size_t measured_sent = 0;
    std::size_t measured_received = 0;

    while (state.phase.load(std::memory_order_acquire) != Phase::stopped) {
        const Phase phase = state.phase.load(std::memory_order_acquire);
        if (phase == Phase::drain) {
            while (measured_received < measured_sent &&
                   state.phase.load(std::memory_order_acquire) != Phase::stopped) {
                if (!co_await readExact(
                        client_socket,
                        asio::buffer(response.data(), response.size()))) {
                    break;
                }
                if (response[0] == kMeasuredMarker) {
                    ++measured_received;
                    addCounter(state.counters.client_received);
                    addCounter(state.counters.client_bytes_received, response.size());
                }
            }
            break;
        }
        const bool measured_frame = phase == Phase::measured;
        payload[0] = measured_frame ? kMeasuredMarker : kWarmupMarker;

        if (!co_await writeAll(client_socket, asio::buffer(payload.data(), payload.size()))) {
            if (state.phase.load(std::memory_order_acquire) < Phase::drain) {
                addCounter(state.counters.runtime_errors);
            }
            break;
        }
        if (measured_frame) {
            ++measured_sent;
            addCounter(state.counters.client_sent);
            addCounter(state.counters.client_bytes_sent, payload.size());
        }

        if (!co_await readExact(client_socket,
                               asio::buffer(response.data(), response.size()))) {
            if (state.phase.load(std::memory_order_acquire) < Phase::drain) {
                addCounter(state.counters.runtime_errors);
            }
            break;
        }
        if (measured_frame && response[0] == kMeasuredMarker &&
            countTraffic(state.phase.load(std::memory_order_acquire))) {
            ++measured_received;
            addCounter(state.counters.client_received);
            addCounter(state.counters.client_bytes_received, response.size());
        }
    }

    boost::system::error_code ignored;
    client_socket.close(ignored);
    state.counters.clients_done.fetch_add(1, std::memory_order_release);
    co_return;
}

awaitable<bool> waitForReady(BenchmarkState& state)
{
    asio::steady_timer timer(state.context);
    for (;;) {
        if (state.counters.clients_ready.load(std::memory_order_acquire) +
                state.counters.clients_failed.load(std::memory_order_acquire) ==
            state.config.clients) {
            co_return state.counters.clients_failed.load(std::memory_order_acquire) == 0;
        }
        timer.expires_after(std::chrono::milliseconds(1));
        boost::system::error_code error;
        co_await timer.async_wait(redirect_error(use_awaitable, error));
        if (error) co_return false;
    }
}

awaitable<bool> waitForConnections(BenchmarkState& state)
{
    asio::steady_timer timer(state.context);
    for (;;) {
        if (state.counters.connections_started.load(std::memory_order_acquire) >=
            state.config.clients) {
            co_return true;
        }
        timer.expires_after(std::chrono::milliseconds(1));
        boost::system::error_code error;
        co_await timer.async_wait(redirect_error(use_awaitable, error));
        if (error) co_return false;
    }
}

awaitable<void> waitForCompletion(BenchmarkState& state)
{
    asio::steady_timer timer(state.context);
    for (;;) {
        const bool clients_done = state.counters.clients_done.load(std::memory_order_acquire) ==
                                   state.config.clients;
        const bool servers_done = state.counters.servers_done.load(std::memory_order_acquire) ==
                                   state.config.workers;
        const bool connections_done =
            state.counters.connections_done.load(std::memory_order_acquire) >=
            state.counters.connections_started.load(std::memory_order_acquire);
        if (clients_done && servers_done && connections_done) co_return;
        timer.expires_after(std::chrono::milliseconds(1));
        boost::system::error_code error;
        co_await timer.async_wait(redirect_error(use_awaitable, error));
        if (error) co_return;
    }
}

awaitable<void> controller(BenchmarkState& state)
{
    for (std::size_t index = 0; index < state.config.clients; ++index) {
        co_spawn(state.context, client(state, index), detached);
    }
    if (!co_await waitForReady(state) || !co_await waitForConnections(state)) {
        state.phase.store(Phase::stopped, std::memory_order_release);
        closeAllSockets(state);
        co_await waitForCompletion(state);
        co_return;
    }

    asio::steady_timer timer(state.context);
    timer.expires_after(state.config.warmup);
    boost::system::error_code error;
    co_await timer.async_wait(redirect_error(use_awaitable, error));
    if (error) co_return;
    if (state.counters.runtime_errors.load(std::memory_order_acquire) != 0) {
        state.phase.store(Phase::stopped, std::memory_order_release);
        closeAllSockets(state);
        co_await waitForCompletion(state);
        co_return;
    }

    resetMeasurementCounters(state);
    state.phase.store(Phase::measured, std::memory_order_release);
    state.measured_started = std::chrono::steady_clock::now();
    timer.expires_after(state.config.duration);
    co_await timer.async_wait(redirect_error(use_awaitable, error));
    if (error) co_return;
    state.measured = snapshot(state);
    state.measured_ended = std::chrono::steady_clock::now();
    state.phase.store(Phase::drain, std::memory_order_release);
    timer.expires_after(kDrainWindow);
    co_await timer.async_wait(redirect_error(use_awaitable, error));
    state.phase.store(Phase::stopped, std::memory_order_release);
    closeAllSockets(state);
    co_await waitForCompletion(state);
    state.settled = snapshot(state);
    co_return;
}

bool prepareServers(BenchmarkState& state)
{
    boost::system::error_code error;
    const auto address = asio::ip::make_address("127.0.0.1", error);
    if (error) return false;
    state.endpoint = tcp::endpoint(address, kServerPort);
    state.acceptors.reserve(state.config.workers);
    for (std::size_t index = 0; index < state.config.workers; ++index) {
        auto acceptor = std::make_unique<tcp::acceptor>(state.context);
        acceptor->open(tcp::v4(), error);
        if (error) return false;
        acceptor->set_option(tcp::acceptor::reuse_address(true), error);
        if (error || !enableReusePort(*acceptor)) return false;
        acceptor->bind(state.endpoint, error);
        if (error) return false;
        acceptor->listen(1024, error);
        if (error) return false;
        state.acceptors.push_back(std::move(acceptor));
    }
    return state.acceptors.size() == state.config.workers;
}

double rate(std::uint64_t count, std::chrono::steady_clock::duration duration)
{
    const double seconds = std::chrono::duration<double>(duration).count();
    return seconds > 0.0 ? static_cast<double>(count) / seconds : 0.0;
}

double lossPercent(const Snapshot& values) noexcept
{
    if (values.client_sent == 0) return 100.0;
    return std::max(0.0, (1.0 - static_cast<double>(values.client_received) /
                               static_cast<double>(values.client_sent)) * 100.0);
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
    state.client_sockets.reserve(config.clients);
    state.server_sockets.reserve(config.clients);
    if (!prepareServers(state)) {
        std::cerr << "boost_asio_coro_tcp: failed to prepare SO_REUSEPORT sockets\n";
        return 1;
    }
    for (auto& acceptor : state.acceptors) {
        co_spawn(context, serverWorker(state, *acceptor), detached);
    }
    co_spawn(context, controller(state), detached);
    const auto started = std::chrono::steady_clock::now();
    context.run();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto measured_duration = state.measured_ended > state.measured_started
        ? state.measured_ended - state.measured_started
        : std::chrono::duration_cast<std::chrono::steady_clock::duration>(config.duration);
    const bool ready = state.counters.clients_ready.load(std::memory_order_acquire) == config.clients;
    const bool complete = state.counters.clients_done.load(std::memory_order_acquire) == config.clients &&
                          state.counters.servers_done.load(std::memory_order_acquire) == config.workers &&
                          state.counters.connections_done.load(std::memory_order_acquire) >=
                              state.counters.connections_started.load(std::memory_order_acquire);
    const bool status_ok = ready && complete && state.measured.client_sent > 0 &&
                           state.measured.runtime_errors == 0 &&
                           state.measured.shutdown_errors == 0 &&
                           state.settled.runtime_errors == 0 &&
                           state.settled.shutdown_errors == 0 &&
                           settledCountersMatch(state.settled);

    std::cout << "meta implementation=boost.asio version=" << boostVersion()
              << " coroutine=co_spawn/awaitable scenario=tcp-echo"
              << " backend=single-io-context clients=" << config.clients
              << " workers=" << config.workers
              << " payload_bytes=" << config.payload_bytes
              << " pipeline=" << kPipeline
              << " warmup_s=" << config.warmup.count()
              << " duration_s=" << config.duration.count()
              << " ready_clients=" << state.counters.clients_ready.load(std::memory_order_acquire)
              << " server_connections=" << state.counters.connections_started.load(std::memory_order_acquire)
              << '\n';
    std::cout << "measured client_sent=" << state.measured.client_sent
              << " client_received=" << state.measured.client_received
              << " client_bytes_sent=" << state.measured.client_bytes_sent
              << " client_bytes_received=" << state.measured.client_bytes_received
              << " server_received=" << state.measured.server_received
              << " server_sent=" << state.measured.server_sent
              << " server_bytes_received=" << state.measured.server_bytes_received
              << " server_bytes_sent=" << state.measured.server_bytes_sent
              << " measurement_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(measured_duration).count()
              << " client_pkt_s=" << rate(state.measured.client_sent, measured_duration)
              << " server_pkt_s=" << rate(state.measured.server_received, measured_duration)
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
              << " elapsed_ms=" << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count()
              << " client_loss_pct=" << lossPercent(state.settled)
              << " settled_loss_pct=" << lossPercent(state.settled)
              << " runtime_errors=" << state.settled.runtime_errors
              << " shutdown_errors=" << state.settled.shutdown_errors << '\n';
    std::cout << "status=" << (status_ok ? "ok" : "fail") << '\n';
    return status_ok ? 0 : 1;
}
