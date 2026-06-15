#include "galay-etcd/sync/etcd_client.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using galay::etcd::EtcdConfig;
using galay::etcd::EtcdClient;

namespace
{

struct Args
{
    std::string endpoint = "http://127.0.0.1:2379";
    int threads = 8;
    int ops_per_thread = 500;
    int value_size = 64;
    bool mixed = false;
};

Args parseArgs(int argc, char** argv)
{
    Args args;
    if (argc > 1) args.endpoint = argv[1];
    if (argc > 2) args.threads = std::max(1, std::stoi(argv[2]));
    if (argc > 3) args.ops_per_thread = std::max(1, std::stoi(argv[3]));
    if (argc > 4) args.value_size = std::max(1, std::stoi(argv[4]));
    if (argc > 5) args.mixed = (std::string(argv[5]) == "mixed");
    return args;
}

std::string payloadOfSize(int size)
{
    return std::string(static_cast<size_t>(size), 'x');
}

double percentile(std::vector<int64_t> samples_us, double p)
{
    if (samples_us.empty()) {
        return 0.0;
    }
    std::sort(samples_us.begin(), samples_us.end());
    const double rank = p * static_cast<double>(samples_us.size() - 1);
    const size_t idx = static_cast<size_t>(rank);
    return static_cast<double>(samples_us[idx]);
}

struct SharedState
{
    explicit SharedState(int worker_count)
        : latency_by_worker(static_cast<size_t>(worker_count))
    {
    }

    std::atomic<int> ready_workers{0};
    std::atomic<int> startup_failures{0};
    std::atomic<int> finished_workers{0};
    std::atomic<int64_t> success{0};
    std::atomic<int64_t> failure{0};
    std::vector<std::vector<int64_t>> latency_by_worker;
    std::mutex error_mutex;
    std::string first_error;
    std::atomic<bool> benchmark_started{false};
    std::atomic<bool> benchmark_aborted{false};
};

void rememberFirstError(SharedState* state, const std::string& error)
{
    if (error.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(state->error_mutex);
    if (state->first_error.empty()) {
        state->first_error = error;
    }
}

bool warmupSession(EtcdClient& session, const std::string& warmup_key, SharedState* state)
{
    auto warmup = session.get(warmup_key);
    if (!warmup.has_value()) {
        rememberFirstError(state, "warmup get failed: " + warmup.error().message());
        return false;
    }
    if (!warmup.value().empty()) {
        rememberFirstError(state, "warmup verification failed");
        return false;
    }
    return true;
}

void cleanupPrefix(const Args& args, const std::string& key_prefix, SharedState* state)
{
    EtcdConfig config;
    config.endpoint = args.endpoint;

    auto session = galay::etcd::EtcdClientBuilder().config(config).build();
    auto conn = session.connect();
    if (!conn.has_value()) {
        rememberFirstError(state, "cleanup connect failed: " + conn.error().message());
        return;
    }

    auto del_result = session.del(key_prefix, true);
    if (!del_result.has_value()) {
        rememberFirstError(state, "cleanup delete failed: " + del_result.error().message());
    }

    (void)session.close();
}

void runWorker(std::string endpoint,
               std::string key_prefix,
               std::string value,
               int worker_id,
               int ops_per_thread,
               bool mixed,
               SharedState* state)
{
    EtcdConfig config;
    config.endpoint = std::move(endpoint);

    auto session = galay::etcd::EtcdClientBuilder().config(config).build();
    auto conn = session.connect();
    if (!conn.has_value()) {
        rememberFirstError(state, "connect failed: " + conn.error().message());
        state->startup_failures.fetch_add(1, std::memory_order_release);
        return;
    }

    if (!warmupSession(session, key_prefix + "warmup/" + std::to_string(worker_id), state)) {
        state->startup_failures.fetch_add(1, std::memory_order_release);
        (void)session.close();
        return;
    }

    state->ready_workers.fetch_add(1, std::memory_order_release);
    while (!state->benchmark_started.load(std::memory_order_acquire) &&
           !state->benchmark_aborted.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (state->benchmark_aborted.load(std::memory_order_acquire)) {
        (void)session.close();
        return;
    }

    for (int i = 0; i < ops_per_thread; ++i) {
        const std::string key = key_prefix + std::to_string(worker_id) + "/" + std::to_string(i);
        const auto begin = std::chrono::steady_clock::now();

        bool ok = false;
        if (mixed) {
            auto put = session.put(key, value);
            if (put.has_value()) {
                auto get = session.get(key);
                ok = get.has_value() &&
                    !get.value().empty() &&
                    get.value().front().value == value;
                if (!ok && !get.has_value()) {
                    rememberFirstError(state, get.error().message());
                } else if (!ok) {
                    rememberFirstError(state, "mixed benchmark verification failed");
                }
            } else {
                rememberFirstError(state, put.error().message());
            }
        } else {
            auto put = session.put(key, value);
            ok = put.has_value();
            if (!ok) {
                rememberFirstError(state, put.error().message());
            }
        }

        const auto end = std::chrono::steady_clock::now();
        const int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count();
        state->latency_by_worker[static_cast<size_t>(worker_id)].push_back(us);

        if (ok) {
            state->success.fetch_add(1, std::memory_order_relaxed);
        } else {
            state->failure.fetch_add(1, std::memory_order_relaxed);
        }
    }

    state->finished_workers.fetch_add(1, std::memory_order_release);
    (void)session.close();
}

} // namespace

int main(int argc, char** argv)
{
    const Args args = parseArgs(argc, argv);
    const std::string value = payloadOfSize(args.value_size);
    const std::string key_prefix = "/galay-etcd/bench/" +
        std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count()) + "/";

    SharedState state(args.threads);
    for (auto& latencies : state.latency_by_worker) {
        latencies.reserve(static_cast<size_t>(args.ops_per_thread));
    }

    std::vector<std::thread> workers;
    workers.reserve(static_cast<size_t>(args.threads));

    for (int worker = 0; worker < args.threads; ++worker) {
        workers.emplace_back(
            runWorker,
            args.endpoint,
            key_prefix,
            value,
            worker,
            args.ops_per_thread,
            args.mixed,
            &state);
    }

    while (state.ready_workers.load(std::memory_order_acquire) +
               state.startup_failures.load(std::memory_order_acquire) < args.threads) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (state.startup_failures.load(std::memory_order_acquire) != 0 ||
        state.ready_workers.load(std::memory_order_acquire) != args.threads) {
        state.benchmark_aborted.store(true, std::memory_order_release);
        for (auto& worker : workers) {
            worker.join();
        }
        std::cerr << "benchmark startup failed";
        if (!state.first_error.empty()) {
            std::cerr << ": " << state.first_error;
        }
        std::cerr << '\n';
        return 1;
    }

    state.benchmark_started.store(true, std::memory_order_release);
    const auto benchmark_begin = std::chrono::steady_clock::now();
    while (state.finished_workers.load(std::memory_order_acquire) < args.threads) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto benchmark_end = std::chrono::steady_clock::now();

    for (auto& worker : workers) {
        worker.join();
    }

    cleanupPrefix(args, key_prefix, &state);

    const double seconds =
        std::chrono::duration_cast<std::chrono::duration<double>>(benchmark_end - benchmark_begin).count();

    std::vector<int64_t> all_latency;
    all_latency.reserve(static_cast<size_t>(args.threads * args.ops_per_thread));
    for (const auto& latencies : state.latency_by_worker) {
        all_latency.insert(all_latency.end(), latencies.begin(), latencies.end());
    }

    const int64_t ok_count = state.success.load(std::memory_order_relaxed);
    const int64_t fail_count = state.failure.load(std::memory_order_relaxed);
    const int64_t total = ok_count + fail_count;
    const double qps = seconds > 0 ? static_cast<double>(ok_count) / seconds : 0.0;

    std::cout << "Endpoint      : " << args.endpoint << '\n';
    std::cout << "Mode          : " << (args.mixed ? "mixed(put+get)" : "put") << '\n';
    std::cout << "Workers       : " << args.threads << '\n';
    std::cout << "Ops/worker    : " << args.ops_per_thread << '\n';
    std::cout << "Value size    : " << args.value_size << " bytes\n";
    std::cout << "Total ops     : " << total << '\n';
    std::cout << "Success       : " << ok_count << '\n';
    std::cout << "Failure       : " << fail_count << '\n';
    std::cout << "Duration      : " << seconds << " s\n";
    std::cout << "Throughput    : " << qps << " ops/s\n";
    std::cout << "Latency p50   : " << percentile(all_latency, 0.50) << " us\n";
    std::cout << "Latency p95   : " << percentile(all_latency, 0.95) << " us\n";
    std::cout << "Latency p99   : " << percentile(all_latency, 0.99) << " us\n";
    std::cout << "Latency max   : "
              << (all_latency.empty() ? 0 : *std::max_element(all_latency.begin(), all_latency.end()))
              << " us\n";
    if (!state.first_error.empty()) {
        std::cout << "First error   : " << state.first_error << '\n';
    }

    return fail_count == 0 ? 0 : 2;
}
