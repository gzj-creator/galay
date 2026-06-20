#ifndef GALAY_MONGO_BENCH_COMMON_H
#define GALAY_MONGO_BENCH_COMMON_H

#include <galay/cpp/galay-mongo/base/mongo_config.h>
#include <galay/cpp/galay-mongo/base/mongo_error.h>

#include <galay/cpp/galay-kernel/core/task.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <expected>
#include <iostream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace mongo_bench
{

template <typename T>
std::expected<T, galay::mongo::MongoError>
unwrapMongoTaskResult(std::expected<std::expected<T, galay::mongo::MongoError>,
                                    galay::kernel::detail::TaskResultError>&& task_result,
                      galay::mongo::MongoErrorType fallback = galay::mongo::MONGO_ERROR_INTERNAL)
{
    if (!task_result) {
        return std::unexpected(galay::mongo::MongoError(
            fallback,
            std::string(task_result.error().message())));
    }
    return std::move(task_result.value());
}

enum class BenchMode
{
    Normal,
    Pipeline,
};

inline const char* modeToString(BenchMode mode)
{
    switch (mode) {
    case BenchMode::Normal:
        return "normal";
    case BenchMode::Pipeline:
        return "pipeline";
    }
    return "normal";
}

inline std::optional<BenchMode> parseMode(std::string_view text)
{
    if (text == "normal") return BenchMode::Normal;
    if (text == "pipeline") return BenchMode::Pipeline;
    return std::nullopt;
}

struct BenchConfig
{
    std::string host = "127.0.0.1";
    uint16_t port = 27017;
    std::string database = "admin";
    std::string username;
    std::string password;
    std::string auth_database = "admin";

    size_t total_requests = 1000;
    size_t concurrency = 20;
    size_t timeout_seconds = 300;
    size_t batch_size = 16;
    size_t buffer_size = 16 * 1024;
    BenchMode mode = BenchMode::Normal;
    bool alloc_stats = false;
};

inline const char* getEnvNonEmpty(const char* key)
{
    const char* value = std::getenv(key);
    if (value == nullptr || value[0] == '\0') {
        return nullptr;
    }
    return value;
}

inline std::string getEnvOrDefault(const char* key, const std::string& fallback)
{
    if (const char* value = getEnvNonEmpty(key)) {
        return value;
    }
    return fallback;
}

inline uint16_t parsePortOrDefault(const char* value, uint16_t fallback)
{
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }

    errno = 0;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0 || parsed > 65535UL) {
        return fallback;
    }
    return static_cast<uint16_t>(parsed);
}

inline size_t parseSizeOrDefault(const char* value, size_t fallback)
{
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }

    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0ULL) {
        return fallback;
    }
    return static_cast<size_t>(parsed);
}

inline bool parseBoolOrDefault(const char* value, bool fallback)
{
    if (value == nullptr || value[0] == '\0') {
        return fallback;
    }

    const std::string_view text(value);
    if (text == "1" || text == "true" || text == "TRUE" || text == "yes" || text == "YES") {
        return true;
    }
    if (text == "0" || text == "false" || text == "FALSE" || text == "no" || text == "NO") {
        return false;
    }
    return fallback;
}

inline BenchConfig loadBenchConfig()
{
    BenchConfig cfg;

    cfg.host = getEnvOrDefault("GALAY_MONGO_HOST", cfg.host);
    cfg.port = parsePortOrDefault(getEnvNonEmpty("GALAY_MONGO_PORT"), cfg.port);
    cfg.database = getEnvOrDefault("GALAY_MONGO_DB", cfg.database);
    cfg.username = getEnvOrDefault("GALAY_MONGO_USER", cfg.username);
    cfg.password = getEnvOrDefault("GALAY_MONGO_PASSWORD", cfg.password);
    cfg.auth_database = getEnvOrDefault("GALAY_MONGO_AUTH_DB", cfg.auth_database);

    cfg.total_requests =
        parseSizeOrDefault(getEnvNonEmpty("GALAY_MONGO_BENCH_TOTAL"), cfg.total_requests);
    cfg.concurrency =
        parseSizeOrDefault(getEnvNonEmpty("GALAY_MONGO_BENCH_CONCURRENCY"), cfg.concurrency);
    cfg.timeout_seconds =
        parseSizeOrDefault(getEnvNonEmpty("GALAY_MONGO_BENCH_TIMEOUT"), cfg.timeout_seconds);
    cfg.batch_size =
        parseSizeOrDefault(getEnvNonEmpty("GALAY_MONGO_BENCH_BATCH_SIZE"), cfg.batch_size);
    cfg.buffer_size =
        parseSizeOrDefault(getEnvNonEmpty("GALAY_MONGO_BENCH_BUFFER_SIZE"), cfg.buffer_size);
    cfg.alloc_stats =
        parseBoolOrDefault(getEnvNonEmpty("GALAY_MONGO_BENCH_ALLOC_STATS"), cfg.alloc_stats);

    if (const char* mode_env = getEnvNonEmpty("GALAY_MONGO_BENCH_MODE")) {
        if (auto mode = parseMode(mode_env); mode.has_value()) {
            cfg.mode = mode.value();
        }
    }

    return cfg;
}

inline bool parseSizeArgValue(int argc, char** argv, int& i, size_t& out)
{
    if (i + 1 >= argc) {
        return false;
    }
    const size_t parsed = parseSizeOrDefault(argv[i + 1], 0);
    if (parsed == 0) {
        return false;
    }
    out = parsed;
    ++i;
    return true;
}

inline bool parsePortArgValue(int argc, char** argv, int& i, uint16_t& out)
{
    if (i + 1 >= argc) {
        return false;
    }
    const uint16_t parsed = parsePortOrDefault(argv[i + 1], 0);
    if (parsed == 0) {
        return false;
    }
    out = parsed;
    ++i;
    return true;
}

inline bool parseArgs(BenchConfig& cfg, int argc, char** argv, std::ostream& err)
{
    std::vector<std::string_view> positional;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (!arg.starts_with("--")) {
            positional.push_back(arg);
            continue;
        }

        if (arg == "--total") {
            if (!parseSizeArgValue(argc, argv, i, cfg.total_requests)) {
                err << "invalid --total value" << std::endl;
                return false;
            }
            continue;
        }
        if (arg == "--concurrency") {
            if (!parseSizeArgValue(argc, argv, i, cfg.concurrency)) {
                err << "invalid --concurrency value" << std::endl;
                return false;
            }
            continue;
        }
        if (arg == "--timeout-sec") {
            if (!parseSizeArgValue(argc, argv, i, cfg.timeout_seconds)) {
                err << "invalid --timeout-sec value" << std::endl;
                return false;
            }
            continue;
        }
        if (arg == "--batch-size") {
            if (!parseSizeArgValue(argc, argv, i, cfg.batch_size)) {
                err << "invalid --batch-size value" << std::endl;
                return false;
            }
            continue;
        }
        if (arg == "--buffer-size") {
            if (!parseSizeArgValue(argc, argv, i, cfg.buffer_size)) {
                err << "invalid --buffer-size value" << std::endl;
                return false;
            }
            continue;
        }
        if (arg == "--host") {
            if (i + 1 >= argc) {
                err << "missing --host value" << std::endl;
                return false;
            }
            cfg.host = argv[++i];
            continue;
        }
        if (arg == "--port") {
            if (!parsePortArgValue(argc, argv, i, cfg.port)) {
                err << "invalid --port value" << std::endl;
                return false;
            }
            continue;
        }
        if (arg == "--db") {
            if (i + 1 >= argc) {
                err << "missing --db value" << std::endl;
                return false;
            }
            cfg.database = argv[++i];
            continue;
        }
        if (arg == "--user") {
            if (i + 1 >= argc) {
                err << "missing --user value" << std::endl;
                return false;
            }
            cfg.username = argv[++i];
            continue;
        }
        if (arg == "--password") {
            if (i + 1 >= argc) {
                err << "missing --password value" << std::endl;
                return false;
            }
            cfg.password = argv[++i];
            continue;
        }
        if (arg == "--auth-db") {
            if (i + 1 >= argc) {
                err << "missing --auth-db value" << std::endl;
                return false;
            }
            cfg.auth_database = argv[++i];
            continue;
        }
        if (arg == "--mode") {
            if (i + 1 >= argc) {
                err << "missing --mode value" << std::endl;
                return false;
            }
            const auto mode = parseMode(argv[++i]);
            if (!mode.has_value()) {
                err << "invalid --mode value, expected normal|pipeline" << std::endl;
                return false;
            }
            cfg.mode = mode.value();
            continue;
        }
        if (arg == "--alloc-stats") {
            cfg.alloc_stats = true;
            continue;
        }

        err << "unknown argument: " << arg << std::endl;
        return false;
    }

    // 兼容旧参数形式:
    // [1]=total [2]=concurrency [3]=host [4]=port [5]=db [6]=user [7]=password [8]=auth_db
    if (!positional.empty()) cfg.total_requests = parseSizeOrDefault(positional[0].data(), cfg.total_requests);
    if (positional.size() > 1) cfg.concurrency = parseSizeOrDefault(positional[1].data(), cfg.concurrency);
    if (positional.size() > 2) cfg.host = std::string(positional[2]);
    if (positional.size() > 3) cfg.port = parsePortOrDefault(positional[3].data(), cfg.port);
    if (positional.size() > 4) cfg.database = std::string(positional[4]);
    if (positional.size() > 5) cfg.username = std::string(positional[5]);
    if (positional.size() > 6) cfg.password = std::string(positional[6]);
    if (positional.size() > 7) cfg.auth_database = std::string(positional[7]);

    if (cfg.concurrency == 0) {
        cfg.concurrency = 1;
    }
    if (cfg.total_requests == 0) {
        cfg.total_requests = 1;
    }
    if (cfg.batch_size == 0) {
        cfg.batch_size = 1;
    }
    if (cfg.buffer_size == 0) {
        cfg.buffer_size = 16 * 1024;
    }

    return true;
}

inline galay::mongo::MongoConfig toMongoConfig(const BenchConfig& cfg)
{
    galay::mongo::MongoConfig mongo_cfg;
    mongo_cfg.host = cfg.host;
    mongo_cfg.port = cfg.port;
    mongo_cfg.database = cfg.database;
    mongo_cfg.username = cfg.username;
    mongo_cfg.password = cfg.password;
    mongo_cfg.auth_database = cfg.auth_database;
    mongo_cfg.app_name = "galay-mongo-benchmark";
    return mongo_cfg;
}

inline void printBenchConfig(const std::string& bench_name, const BenchConfig& cfg)
{
    std::cout << "[" << bench_name << "]"
              << " host=" << cfg.host
              << " port=" << cfg.port
              << " db=" << cfg.database
              << " user=" << (cfg.username.empty() ? "<empty>" : cfg.username)
              << " total=" << cfg.total_requests
              << " concurrency=" << cfg.concurrency
              << " mode=" << modeToString(cfg.mode)
              << " batch_size=" << cfg.batch_size
              << " buffer_size=" << cfg.buffer_size
              << " timeout_sec=" << cfg.timeout_seconds
              << " alloc_stats=" << (cfg.alloc_stats ? "on" : "off")
              << std::endl;
}

inline double percentile(std::vector<double> values, double p)
{
    if (values.empty()) {
        return 0.0;
    }

    if (p <= 0.0) {
        p = 0.0;
    }
    if (p >= 1.0) {
        p = 1.0;
    }

    std::sort(values.begin(), values.end());
    const double idx = p * static_cast<double>(values.size() - 1);
    const size_t lo = static_cast<size_t>(std::floor(idx));
    const size_t hi = static_cast<size_t>(std::ceil(idx));

    if (lo == hi) {
        return values[lo];
    }

    const double weight = idx - static_cast<double>(lo);
    return values[lo] * (1.0 - weight) + values[hi] * weight;
}

inline void printBenchReport(size_t total,
                             size_t ok,
                             size_t error,
                             long long duration_ms,
                             const std::vector<double>& latencies_ms,
                             bool alloc_stats_enabled = false,
                             uint64_t alloc_count = 0,
                             uint64_t alloc_bytes = 0)
{
    const double seconds = static_cast<double>(duration_ms) / 1000.0;
    const double rps = seconds <= 0.0 ? 0.0 : static_cast<double>(ok) / seconds;
    const double p50 = percentile(latencies_ms, 0.50);
    const double p95 = percentile(latencies_ms, 0.95);
    const double p99 = percentile(latencies_ms, 0.99);
    const double max = percentile(latencies_ms, 1.00);

    std::cout << "Total requests: " << total << "\n"
              << "Success: " << ok << "\n"
              << "Errors: " << error << "\n"
              << "Duration(ms): " << duration_ms << "\n"
              << "Requests/sec: " << rps << "\n"
              << "Latency p50(ms): " << p50 << "\n"
              << "Latency p95(ms): " << p95 << "\n"
              << "Latency p99(ms): " << p99 << "\n"
              << "Latency max(ms): " << max << std::endl;

    if (alloc_stats_enabled && total > 0) {
        std::cout << "Avg allocs/op: "
                  << static_cast<double>(alloc_count) / static_cast<double>(total) << "\n"
                  << "Avg alloc bytes/op: "
                  << static_cast<double>(alloc_bytes) / static_cast<double>(total)
                  << std::endl;
    }
}

inline void printUsage(const char* prog)
{
    std::cout
        << "Usage: " << prog
        << " [--total N] [--concurrency N] [--timeout-sec N]"
        << " [--mode normal|pipeline] [--batch-size N] [--buffer-size N] [--alloc-stats]\n"
        << "             [--host H] [--port P] [--db DB] [--user U] [--password PW] [--auth-db DB]\n"
        << "Legacy positional args remain supported:\n"
        << "  " << prog << " [total] [concurrency] [host] [port] [db] [user] [password] [auth_db]\n"
        << "Environment overrides:\n"
        << "  GALAY_MONGO_HOST / GALAY_MONGO_PORT / GALAY_MONGO_DB / GALAY_MONGO_USER\n"
        << "  GALAY_MONGO_PASSWORD / GALAY_MONGO_AUTH_DB\n"
        << "  GALAY_MONGO_BENCH_TOTAL / GALAY_MONGO_BENCH_CONCURRENCY / GALAY_MONGO_BENCH_TIMEOUT\n"
        << "  GALAY_MONGO_BENCH_MODE / GALAY_MONGO_BENCH_BATCH_SIZE / GALAY_MONGO_BENCH_BUFFER_SIZE\n"
        << "  GALAY_MONGO_BENCH_ALLOC_STATS\n";
}

} // namespace mongo_bench

#endif // GALAY_MONGO_BENCH_COMMON_H
