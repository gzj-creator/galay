#include <hiredis/hiredis.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{

struct BenchmarkOptions
{
    std::string host = "127.0.0.1";
    int port = 6379;
    int clients = 10;
    int operations = 100;
    int batch_size = 100;
    std::string mode = "normal";
    bool verbose = true;
};

template <typename Integer>
bool parseInteger(std::string_view text, Integer& value)
{
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto [parsed_end, error] = std::from_chars(begin, end, value);
    return error == std::errc{} && parsed_end == end;
}

void printUsage(const char* program)
{
    std::cout << "Usage: " << program
              << " [-h host] [-p port] [-c clients] [-n operations]"
                 " [-m normal|pipeline] [-b batch_size] [-q]\n";
}

bool parseArgs(int argc, char* argv[], BenchmarkOptions& options, bool& show_help)
{
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--help") {
            show_help = true;
            return false;
        }
        if (arg == "-q") {
            options.verbose = false;
            continue;
        }
        if (i + 1 >= argc) {
            std::cerr << "Missing value for " << arg << '\n';
            return false;
        }

        const std::string_view value(argv[++i]);
        if (arg == "-h") {
            options.host = value;
        } else if (arg == "-p") {
            if (!parseInteger(value, options.port) || options.port <= 0 || options.port > 65535) {
                return false;
            }
        } else if (arg == "-c") {
            if (!parseInteger(value, options.clients) || options.clients <= 0) {
                return false;
            }
        } else if (arg == "-n") {
            if (!parseInteger(value, options.operations) || options.operations <= 0) {
                return false;
            }
        } else if (arg == "-b") {
            if (!parseInteger(value, options.batch_size) || options.batch_size <= 0) {
                return false;
            }
        } else if (arg == "-m") {
            options.mode = value;
            if (options.mode != "normal" && options.mode != "pipeline") {
                return false;
            }
        } else {
            std::cerr << "Unknown argument: " << arg << '\n';
            return false;
        }
    }
    return true;
}

bool replyIsStatus(const redisReply* reply, std::string_view expected)
{
    return reply != nullptr && reply->type == REDIS_REPLY_STATUS && reply->str != nullptr &&
           std::string_view(reply->str, reply->len) == expected;
}

bool runCommand(redisContext* context,
                const char* format,
                const std::string& key,
                const std::string& value,
                bool expect_value)
{
    auto* reply = static_cast<redisReply*>(
        redisCommand(context, format, key.data(), key.size(), value.data(), value.size()));
    if (reply == nullptr) {
        return false;
    }

    bool ok = replyIsStatus(reply, "OK");
    if (expect_value) {
        ok = reply->type == REDIS_REPLY_STRING && reply->str != nullptr &&
             std::string_view(reply->str, reply->len) == value;
    }
    freeReplyObject(reply);
    return ok;
}

void runWorker(const BenchmarkOptions* options,
               int worker_id,
               std::atomic<std::int64_t>* success,
               std::atomic<std::int64_t>* error,
               std::vector<std::int64_t>* latencies)
{
    redisContext* context = redisConnect(options->host.c_str(), options->port);
    if (context == nullptr || context->err != 0) {
        const std::int64_t failed = options->mode == "normal"
            ? static_cast<std::int64_t>(options->operations) * 2
            : options->operations;
        error->fetch_add(failed, std::memory_order_relaxed);
        if (context != nullptr) {
            redisFree(context);
        }
        return;
    }

    auto& samples = latencies[worker_id];
    const std::size_t sample_count = options->mode == "normal"
        ? static_cast<std::size_t>(options->operations) * 2U
        : static_cast<std::size_t>(
              (options->operations + options->batch_size - 1) / options->batch_size);
    samples.reserve(sample_count);
    std::vector<std::string> keys;
    std::vector<std::string> values;
    keys.reserve(static_cast<std::size_t>(options->operations));
    values.reserve(static_cast<std::size_t>(options->operations));
    const std::string prefix = options->mode == "normal"
        ? "bench:hiredis:normal:"
        : "bench:hiredis:pipeline:";
    for (int operation = 0; operation < options->operations; ++operation) {
        keys.push_back(prefix + std::to_string(worker_id) + ':' + std::to_string(operation));
        values.push_back("value_" + std::to_string(operation));
    }

    if (options->mode == "normal") {
        for (int operation = 0; operation < options->operations; ++operation) {
            const auto& key = keys[static_cast<std::size_t>(operation)];
            const auto& value = values[static_cast<std::size_t>(operation)];

            auto begin = std::chrono::steady_clock::now();
            const bool set_ok = runCommand(context, "SET %b %b", key, value, false);
            auto end = std::chrono::steady_clock::now();
            samples.push_back(
                std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count());
            (set_ok ? success : error)->fetch_add(1, std::memory_order_relaxed);

            begin = std::chrono::steady_clock::now();
            const bool get_ok = runCommand(context, "GET %b", key, value, true);
            end = std::chrono::steady_clock::now();
            samples.push_back(
                std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count());
            (get_ok ? success : error)->fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        int offset = 0;
        while (offset < options->operations) {
            const int current_batch = std::min(options->batch_size, options->operations - offset);
            const auto begin = std::chrono::steady_clock::now();
            int appended = 0;
            for (int index = 0; index < current_batch; ++index) {
                const auto input_index = static_cast<std::size_t>(offset + index);
                const auto& key = keys[input_index];
                const auto& value = values[input_index];
                if (redisAppendCommand(
                        context, "SET %b %b", key.data(), key.size(), value.data(), value.size()) == REDIS_OK) {
                    ++appended;
                }
            }

            int batch_success = 0;
            for (int index = 0; index < appended; ++index) {
                void* raw_reply = nullptr;
                const int reply_status = redisGetReply(context, &raw_reply);
                auto* reply = static_cast<redisReply*>(raw_reply);
                if (reply_status == REDIS_OK && replyIsStatus(reply, "OK")) {
                    ++batch_success;
                }
                if (reply != nullptr) {
                    freeReplyObject(reply);
                }
            }
            const auto end = std::chrono::steady_clock::now();
            samples.push_back(
                std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count());
            success->fetch_add(batch_success, std::memory_order_relaxed);
            error->fetch_add(current_batch - batch_success, std::memory_order_relaxed);
            offset += current_batch;
        }
    }

    redisFree(context);
}

std::int64_t percentile(const std::vector<std::int64_t>& sorted_samples, double fraction)
{
    if (sorted_samples.empty()) {
        return 0;
    }
    const auto index = static_cast<std::size_t>(std::ceil(
        fraction * static_cast<double>(sorted_samples.size() - 1)));
    return sorted_samples[index];
}

} // namespace

int main(int argc, char* argv[])
{
    BenchmarkOptions options;
    bool show_help = false;
    if (!parseArgs(argc, argv, options, show_help)) {
        printUsage(argv[0]);
        return show_help ? 0 : 2;
    }

    std::atomic<std::int64_t> success{0};
    std::atomic<std::int64_t> error{0};
    std::vector<std::vector<std::int64_t>> worker_latencies(
        static_cast<std::size_t>(options.clients));
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(options.clients));

    const auto begin = std::chrono::steady_clock::now();
    for (int worker = 0; worker < options.clients; ++worker) {
        workers.emplace_back(
            runWorker, &options, worker, &success, &error, worker_latencies.data());
    }
    for (auto& worker : workers) {
        worker.join();
    }
    const auto end = std::chrono::steady_clock::now();

    std::vector<std::int64_t> latencies;
    for (const auto& samples : worker_latencies) {
        latencies.insert(latencies.end(), samples.begin(), samples.end());
    }
    std::sort(latencies.begin(), latencies.end());

    const auto success_count = success.load(std::memory_order_relaxed);
    const auto error_count = error.load(std::memory_order_relaxed);
    const double seconds = std::chrono::duration<double>(end - begin).count();
    const double ops_per_second = seconds > 0.0
        ? static_cast<double>(success_count) / seconds
        : 0.0;

    if (options.verbose) {
        std::cout << "Implementation: hiredis\n";
    }
    std::cout << "Mode: " << options.mode << '\n'
              << "Clients: " << options.clients << '\n'
              << "Operations per client: " << options.operations << '\n'
              << "Batch size: " << options.batch_size << '\n'
              << "Success: " << success_count << '\n'
              << "Error: " << error_count << '\n'
              << "Timeout: 0\n"
              << "Duration: " << seconds * 1000.0 << "ms\n"
              << "Ops/sec: " << static_cast<std::int64_t>(ops_per_second) << '\n'
              << "Request latency p50 (us): " << percentile(latencies, 0.50) << '\n'
              << "Request latency p99 (us): " << percentile(latencies, 0.99) << '\n';

    return error_count == 0 ? 0 : 1;
}
