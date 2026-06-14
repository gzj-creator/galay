#ifndef GALAY_MYSQL_BENCHMARK_CONFIG_H
#define GALAY_MYSQL_BENCHMARK_CONFIG_H

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>

namespace mysql_benchmark
{

enum class BenchmarkMode
{
    Normal,
    Batch,
    Pipeline
};

inline const char* modeToString(BenchmarkMode mode)
{
    switch (mode) {
    case BenchmarkMode::Normal:
        return "normal";
    case BenchmarkMode::Batch:
        return "batch";
    case BenchmarkMode::Pipeline:
        return "pipeline";
    }
    return "normal";
}

inline std::optional<BenchmarkMode> parseMode(std::string_view mode)
{
    if (mode == "normal") return BenchmarkMode::Normal;
    if (mode == "batch") return BenchmarkMode::Batch;
    if (mode == "pipeline") return BenchmarkMode::Pipeline;
    return std::nullopt;
}

struct MysqlBenchmarkConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 3306;
    std::string user = "root";
    std::string password = "password";
    std::string database = "test";

    size_t clients = 16;
    size_t queries_per_client = 1000;
    size_t warmup_queries = 10;
    size_t timeout_seconds = 180;
    std::string sql = "SELECT 1";
    BenchmarkMode mode = BenchmarkMode::Normal;
    size_t batch_size = 16;
    size_t buffer_size = 16 * 1024;
    bool alloc_stats = false;
};

inline const char* getEnvIfSet(const char* key)
{
    return std::getenv(key);
}

inline const char* getEnvNonEmpty(const char* key)
{
    const char* value = getEnvIfSet(key);
    if (value == nullptr || value[0] == '\0') {
        return nullptr;
    }
    return value;
}

inline std::string getEnvOrDefault(const char* key1, const char* key2, const std::string& default_value)
{
    if (const char* value = getEnvIfSet(key1)) return value;
    if (const char* value = getEnvIfSet(key2)) return value;
    return default_value;
}

inline uint16_t parsePortOrDefault(const char* value, uint16_t default_value)
{
    if (value == nullptr || value[0] == '\0') {
        return default_value;
    }

    errno = 0;
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0 || parsed > 65535UL) {
        return default_value;
    }
    return static_cast<uint16_t>(parsed);
}

inline size_t parseSizeOrDefault(const char* value, size_t default_value)
{
    if (value == nullptr || value[0] == '\0') {
        return default_value;
    }

    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0ULL) {
        return default_value;
    }
    return static_cast<size_t>(parsed);
}

inline size_t getEnvSizeOrDefault(const char* key1, const char* key2, size_t default_value)
{
    if (const char* value = getEnvNonEmpty(key1)) return parseSizeOrDefault(value, default_value);
    if (const char* value = getEnvNonEmpty(key2)) return parseSizeOrDefault(value, default_value);
    return default_value;
}

inline uint16_t getEnvPortOrDefault(const char* key1, const char* key2, uint16_t default_value)
{
    if (const char* value = getEnvNonEmpty(key1)) return parsePortOrDefault(value, default_value);
    if (const char* value = getEnvNonEmpty(key2)) return parsePortOrDefault(value, default_value);
    return default_value;
}

inline bool parseBoolOrDefault(const char* value, bool default_value)
{
    if (value == nullptr || value[0] == '\0') {
        return default_value;
    }
    const std::string_view text(value);
    if (text == "1" || text == "true" || text == "TRUE" || text == "yes" || text == "YES") {
        return true;
    }
    if (text == "0" || text == "false" || text == "FALSE" || text == "no" || text == "NO") {
        return false;
    }
    return default_value;
}

inline MysqlBenchmarkConfig loadMysqlBenchmarkConfig()
{
    MysqlBenchmarkConfig cfg;

    cfg.host = getEnvOrDefault("GALAY_MYSQL_HOST", "MYSQL_HOST", cfg.host);
    cfg.port = getEnvPortOrDefault("GALAY_MYSQL_PORT", "MYSQL_PORT", cfg.port);
    cfg.user = getEnvOrDefault("GALAY_MYSQL_USER", "MYSQL_USER", cfg.user);
    cfg.password = getEnvOrDefault("GALAY_MYSQL_PASSWORD", "MYSQL_PASSWORD", cfg.password);
    cfg.database = getEnvOrDefault("GALAY_MYSQL_DB", "MYSQL_DATABASE", cfg.database);

    cfg.clients = getEnvSizeOrDefault("GALAY_MYSQL_BENCH_CLIENTS", "MYSQL_BENCH_CLIENTS", cfg.clients);
    cfg.queries_per_client = getEnvSizeOrDefault("GALAY_MYSQL_BENCH_QUERIES", "MYSQL_BENCH_QUERIES", cfg.queries_per_client);
    cfg.warmup_queries = getEnvSizeOrDefault("GALAY_MYSQL_BENCH_WARMUP", "MYSQL_BENCH_WARMUP", cfg.warmup_queries);
    cfg.timeout_seconds = getEnvSizeOrDefault("GALAY_MYSQL_BENCH_TIMEOUT", "MYSQL_BENCH_TIMEOUT", cfg.timeout_seconds);
    cfg.sql = getEnvOrDefault("GALAY_MYSQL_BENCH_SQL", "MYSQL_BENCH_SQL", cfg.sql);
    if (const char* mode_env = getEnvNonEmpty("GALAY_MYSQL_BENCH_MODE")) {
        if (auto mode = parseMode(mode_env); mode.has_value()) {
            cfg.mode = mode.value();
        }
    }
    cfg.batch_size = getEnvSizeOrDefault("GALAY_MYSQL_BENCH_BATCH_SIZE", "MYSQL_BENCH_BATCH_SIZE", cfg.batch_size);
    cfg.buffer_size = getEnvSizeOrDefault("GALAY_MYSQL_BENCH_BUFFER_SIZE", "MYSQL_BENCH_BUFFER_SIZE", cfg.buffer_size);
    cfg.alloc_stats = parseBoolOrDefault(getEnvNonEmpty("GALAY_MYSQL_BENCH_ALLOC_STATS"), cfg.alloc_stats);

    return cfg;
}

inline bool parsePositiveSizeArg(int argc, char* argv[], int& i, size_t& out)
{
    if (i + 1 >= argc) {
        return false;
    }

    errno = 0;
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(argv[i + 1], &end, 10);
    if (errno != 0 || end == argv[i + 1] || *end != '\0' || parsed == 0ULL) {
        return false;
    }

    out = static_cast<size_t>(parsed);
    ++i;
    return true;
}

inline bool parseArgs(MysqlBenchmarkConfig& cfg, int argc, char* argv[], std::ostream& err)
{
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);

        if (arg == "--clients") {
            if (!parsePositiveSizeArg(argc, argv, i, cfg.clients)) {
                err << "invalid --clients value" << std::endl;
                return false;
            }
            continue;
        }

        if (arg == "--queries") {
            if (!parsePositiveSizeArg(argc, argv, i, cfg.queries_per_client)) {
                err << "invalid --queries value" << std::endl;
                return false;
            }
            continue;
        }

        if (arg == "--warmup") {
            if (!parsePositiveSizeArg(argc, argv, i, cfg.warmup_queries)) {
                err << "invalid --warmup value" << std::endl;
                return false;
            }
            continue;
        }

        if (arg == "--timeout-sec") {
            if (!parsePositiveSizeArg(argc, argv, i, cfg.timeout_seconds)) {
                err << "invalid --timeout-sec value" << std::endl;
                return false;
            }
            continue;
        }

        if (arg == "--sql") {
            if (i + 1 >= argc) {
                err << "missing --sql value" << std::endl;
                return false;
            }
            cfg.sql = argv[++i];
            if (cfg.sql.empty()) {
                err << "empty --sql value" << std::endl;
                return false;
            }
            continue;
        }

        if (arg == "--mode") {
            if (i + 1 >= argc) {
                err << "missing --mode value" << std::endl;
                return false;
            }
            const auto mode = parseMode(argv[++i]);
            if (!mode.has_value()) {
                err << "invalid --mode value, expected normal|batch|pipeline" << std::endl;
                return false;
            }
            cfg.mode = mode.value();
            continue;
        }

        if (arg == "--batch-size") {
            if (!parsePositiveSizeArg(argc, argv, i, cfg.batch_size)) {
                err << "invalid --batch-size value" << std::endl;
                return false;
            }
            continue;
        }

        if (arg == "--buffer-size") {
            if (!parsePositiveSizeArg(argc, argv, i, cfg.buffer_size)) {
                err << "invalid --buffer-size value" << std::endl;
                return false;
            }
            continue;
        }

        if (arg == "--alloc-stats") {
            cfg.alloc_stats = true;
            continue;
        }

        err << "unknown argument: " << arg << std::endl;
        return false;
    }

    return true;
}

inline void printUsage(const char* prog)
{
    std::cout
        << "Usage: " << prog
        << " [--clients N] [--queries N] [--warmup N] [--timeout-sec N]"
        << " [--sql \"SELECT 1\"] [--mode normal|batch|pipeline]"
        << " [--batch-size N] [--buffer-size N] [--alloc-stats]\n"
        << "Environment overrides:\n"
        << "  GALAY_MYSQL_HOST / GALAY_MYSQL_PORT / GALAY_MYSQL_USER / GALAY_MYSQL_PASSWORD / GALAY_MYSQL_DB\n"
        << "  GALAY_MYSQL_BENCH_CLIENTS / GALAY_MYSQL_BENCH_QUERIES / GALAY_MYSQL_BENCH_WARMUP\n"
        << "  GALAY_MYSQL_BENCH_TIMEOUT / GALAY_MYSQL_BENCH_SQL / GALAY_MYSQL_BENCH_MODE\n"
        << "  GALAY_MYSQL_BENCH_BATCH_SIZE / GALAY_MYSQL_BENCH_BUFFER_SIZE\n"
        << "  GALAY_MYSQL_BENCH_ALLOC_STATS\n";
}

inline void printConfig(const MysqlBenchmarkConfig& cfg)
{
    std::cout
        << "MySQL config: host=" << cfg.host
        << ", port=" << cfg.port
        << ", user=" << cfg.user
        << ", db=" << cfg.database << '\n'
        << "Benchmark config: clients=" << cfg.clients
        << ", queries_per_client=" << cfg.queries_per_client
        << ", warmup=" << cfg.warmup_queries
        << ", timeout_sec=" << cfg.timeout_seconds
        << ", mode=" << modeToString(cfg.mode)
        << ", batch_size=" << cfg.batch_size
        << ", buffer_size=" << cfg.buffer_size
        << ", alloc_stats=" << (cfg.alloc_stats ? "on" : "off") << '\n'
        << "SQL: " << cfg.sql << std::endl;
}

} // namespace mysql_benchmark

#endif // GALAY_MYSQL_BENCHMARK_CONFIG_H
