#ifndef GALAY_POSTGRES_BENCHMARK_CONFIG_H
#define GALAY_POSTGRES_BENCHMARK_CONFIG_H

#include <charconv>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace postgres_benchmark
{

struct Config
{
    std::string host = "127.0.0.1";
    std::string user = "postgres";
    std::string password = "postgres";
    std::string database = "postgres";
    std::string sql = "SELECT 1";
    size_t clients = 4;
    size_t queries = 1000;
    size_t pool_size = 4;
    uint16_t port = 5432;
};

inline std::string environment_or(const char* name, std::string fallback)
{
    const char* value = std::getenv(name);
    return value != nullptr && value[0] != '\0' ? std::string(value) : fallback;
}

template<typename Integer>
bool parse_positive(std::string_view text, Integer* output)
{
    Integer value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || parsed.ec != std::errc{} ||
        parsed.ptr != text.data() + text.size() || value == 0) {
        return false;
    }
    *output = value;
    return true;
}

inline Config load_config()
{
    Config config;
    config.host = environment_or("GALAY_POSTGRES_HOST", config.host);
    config.user = environment_or("GALAY_POSTGRES_USER", config.user);
    config.password = environment_or("GALAY_POSTGRES_PASSWORD", config.password);
    config.database = environment_or("GALAY_POSTGRES_DB", config.database);
    config.sql = environment_or("GALAY_POSTGRES_BENCH_SQL", config.sql);
    const std::string port = environment_or("GALAY_POSTGRES_PORT", "5432");
    (void)parse_positive(port, &config.port);
    const std::string clients = environment_or("GALAY_POSTGRES_BENCH_CLIENTS", "4");
    const std::string queries = environment_or("GALAY_POSTGRES_BENCH_QUERIES", "1000");
    const std::string pool_size = environment_or("GALAY_POSTGRES_BENCH_POOL_SIZE", "4");
    (void)parse_positive(clients, &config.clients);
    (void)parse_positive(queries, &config.queries);
    (void)parse_positive(pool_size, &config.pool_size);
    return config;
}

inline bool parse_args(Config& config, int argc, char** argv)
{
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (index + 1 >= argc) {
            return false;
        }
        const std::string_view value(argv[++index]);
        if (argument == "--clients") {
            if (!parse_positive(value, &config.clients)) return false;
        } else if (argument == "--queries") {
            if (!parse_positive(value, &config.queries)) return false;
        } else if (argument == "--pool-size") {
            if (!parse_positive(value, &config.pool_size)) return false;
        } else if (argument == "--sql") {
            if (value.empty()) return false;
            config.sql.assign(value);
        } else {
            return false;
        }
    }
    return true;
}

inline void print_usage(const char* program)
{
    std::cerr << "usage: " << program
              << " [--clients N] [--queries N] [--pool-size N] [--sql SQL]\n";
}

inline void print_config(const Config& config)
{
    std::cout << "host=" << config.host << " port=" << config.port
              << " user=" << config.user << " database=" << config.database
              << " clients=" << config.clients << " queries=" << config.queries
              << " pool_size=" << config.pool_size << " sql=" << config.sql << '\n';
}

} // namespace postgres_benchmark

#endif // GALAY_POSTGRES_BENCHMARK_CONFIG_H
