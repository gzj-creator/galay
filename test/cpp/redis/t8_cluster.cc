#include <galay/cpp/galay-kernel/core/runtime.h>
#include <galay/cpp/galay-kernel/common/sleep.hpp>
#include "integration_config.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <string>

#include <galay/cpp/galay-redis/async/client.h>
#include <galay/cpp/galay-redis/async/topology_client.h>

using namespace galay::kernel;
using namespace galay::redis;

namespace
{
    struct IntegrationConfig
    {
        bool enabled = false;
        std::string cluster_host = "127.0.0.1";
        int32_t cluster_port = 7000;
        std::string moved_key;
        std::string ask_key;
        std::string sentinel_host = "127.0.0.1";
        int32_t sentinel_port = 26379;
        std::string sentinel_master_name = "mymaster";
        std::string sentinel_test_key = "galay:it:sentinel:key";
        bool trigger_sentinel_failover = true;
    };

    std::mutex g_mutex;
    std::condition_variable g_cv;
    bool g_done = false;
    std::atomic<int> g_failures{0};

    bool parse_int(const char* s, int32_t* out)
    {
        if (!s || !out) {
            return false;
        }
        try {
            *out = std::stoi(s);
            return true;
        } catch (...) {
            return false;
        }
    }

    bool parse_bool(const char* s, bool default_value)
    {
        if (!s) {
            return default_value;
        }
        std::string v(s);
        for (auto& c : v) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return v == "1" || v == "true" || v == "yes" || v == "on";
    }

    IntegrationConfig load_config()
    {
        IntegrationConfig cfg;
        cfg.enabled = parse_bool(std::getenv("GALAY_IT_ENABLE"), false);
        if (const char* v = std::getenv("GALAY_IT_CLUSTER_HOST")) {
            cfg.cluster_host = v;
        }
        if (const char* v = std::getenv("GALAY_IT_CLUSTER_PORT")) {
            int32_t p = 0;
            if (parse_int(v, &p)) {
                cfg.cluster_port = p;
            }
        }
        if (const char* v = std::getenv("GALAY_IT_MOVED_KEY")) {
            cfg.moved_key = v;
        }
        if (const char* v = std::getenv("GALAY_IT_ASK_KEY")) {
            cfg.ask_key = v;
        }
        if (const char* v = std::getenv("GALAY_IT_SENTINEL_HOST")) {
            cfg.sentinel_host = v;
        }
        if (const char* v = std::getenv("GALAY_IT_SENTINEL_PORT")) {
            int32_t p = 0;
            if (parse_int(v, &p)) {
                cfg.sentinel_port = p;
            }
        }
        if (const char* v = std::getenv("GALAY_IT_SENTINEL_MASTER_NAME")) {
            cfg.sentinel_master_name = v;
        }
        if (const char* v = std::getenv("GALAY_IT_SENTINEL_KEY")) {
            cfg.sentinel_test_key = v;
        }
        cfg.trigger_sentinel_failover = parse_bool(std::getenv("GALAY_IT_TRIGGER_SENTINEL_FAILOVER"), true);
        return cfg;
    }

    void fail(const std::string& msg)
    {
        ++g_failures;
        std::cerr << "[FAILED] " << msg << std::endl;
    }

    bool expect_single_string_reply(const RedisCommandResult& result, std::string* out)
    {
        if (!result) {
            fail("Command failed: " + std::string(result.error().message()));
            return false;
        }
        if (result.value().empty()) {
            fail("Reply empty");
            return false;
        }
        const auto& first = result.value().front();
        if (first.is_string()) {
            *out = first.to_string();
            return true;
        }
        if (first.is_status()) {
            *out = first.to_status();
            return true;
        }
        fail("Reply is not string/status");
        return false;
    }

    bool expect_awaited_single_string_reply(auto&& result, std::string* out)
    {
        if (!result) {
            fail("Command failed: " + std::string(result.error().message()));
            return false;
        }
        if (!result.value()) {
            fail("Command failed: " + std::string(result.value().error().message()));
            return false;
        }
        return expect_single_string_reply(result.value(), out);
    }

    bool try_awaited_single_string_reply(auto&& result, std::string* out)
    {
        if (!result || !result.value() || !result.value().has_value() ||
            result.value()->empty()) {
            return false;
        }
        const auto& first = result.value()->front();
        if (first.is_string()) {
            *out = first.to_string();
            return true;
        }
        if (first.is_status()) {
            *out = first.to_status();
            return true;
        }
        return false;
    }

    bool try_awaited_status(auto&& result, const std::string& expected)
    {
        std::string value;
        return try_awaited_single_string_reply(result, &value) && value == expected;
    }

    bool is_ask_error_reply(const std::expected<std::optional<std::vector<RedisValue>>, RedisError>& result)
    {
        if (!result || !result.value().has_value() || result.value()->empty()) {
            return false;
        }
        const auto& first = result.value()->front();
        if (!first.is_error()) {
            return false;
        }
        const auto msg = first.to_error();
        return msg.find("ASK ") != std::string::npos || msg.find("ASK\t") != std::string::npos;
    }
}

Task<void> run_integration(IOScheduler* scheduler, IntegrationConfig cfg)
{
    RedisCommandBuilder command_builder;
    do {
        auto cluster = RedisClusterClientBuilder().scheduler(scheduler).build();
        cluster.set_auto_refresh_interval(std::chrono::milliseconds(1000));

        RedisClusterNodeAddress seed;
        seed.host = cfg.cluster_host;
        seed.port = cfg.cluster_port;
        seed.slot_start = 0;
        seed.slot_end = 16383;

        auto seed_connect = co_await cluster.add_node(seed);
        if (!seed_connect) {
            fail("Cluster seed connect failed: " + std::string(seed_connect.error().message()));
            break;
        }

        auto slots_refresh = co_await cluster.refresh_slots();
        if (!slots_refresh) {
            fail("Cluster refreshSlots failed: " + std::string(slots_refresh.error().message()));
            break;
        }

        std::string moved_key = cfg.moved_key.empty() ? "galay:it:moved:key" : cfg.moved_key;
        cluster.set_slot_range(0, 0, 16383);  // 故意污染本地映射，触发 MOVED 自动重定向路径

        auto moved_set = co_await cluster.execute("SET", {moved_key, "moved-ok"}, moved_key, true);
        if (!moved_set) {
            fail("MOVED auto redirect SET failed: " + std::string(moved_set.error().message()));
            break;
        }
        auto moved_get = co_await cluster.execute("GET", {moved_key}, moved_key, true);
        std::string moved_value;
        if (!expect_awaited_single_string_reply(moved_get, &moved_value)) {
            break;
        }
        if (moved_value != "moved-ok") {
            fail("MOVED auto redirect value mismatch");
            break;
        }

        if (!cfg.ask_key.empty()) {
            auto direct_seed = RedisClientBuilder().scheduler(scheduler).build();
            auto direct_connect = co_await direct_seed.connect(cfg.cluster_host, cfg.cluster_port);
            if (!direct_connect) {
                fail("Direct seed connect failed: " + std::string(direct_connect.error().message()));
                break;
            }

            auto ask_raw = co_await direct_seed.command(command_builder.get(cfg.ask_key))
                               .timeout(std::chrono::seconds(5));
            if (!is_ask_error_reply(ask_raw)) {
                fail("ASK not observed on direct seed GET, check container ask-slot setup");
                break;
            }

            auto ask_auto = co_await cluster.execute("GET", {cfg.ask_key}, cfg.ask_key, true);
            if (!ask_auto) {
                fail("ASK auto redirect failed: " + std::string(ask_auto.error().message()));
                break;
            }

            co_await direct_seed.close();
        }

        auto ms = RedisMasterSlaveClientBuilder().scheduler(scheduler).build();
        ms.set_sentinel_master_name(cfg.sentinel_master_name);

        RedisNodeAddress sentinel;
        sentinel.host = cfg.sentinel_host;
        sentinel.port = cfg.sentinel_port;
        auto sentinel_connect = co_await ms.add_sentinel(sentinel);
        if (!sentinel_connect) {
            fail("Sentinel connect failed: " + std::string(sentinel_connect.error().message()));
            break;
        }

        bool sentinel_refresh_ok = false;
        for (int i = 0; i < 20; ++i) {
            auto refresh = co_await ms.refresh_from_sentinel();
            if (refresh) {
                sentinel_refresh_ok = true;
                break;
            }
            co_await galay::kernel::sleep(std::chrono::milliseconds(500));
        }
        if (!sentinel_refresh_ok) {
            fail("Sentinel refresh failed after retries");
            break;
        }

        auto write_before = co_await ms.execute("SET", {cfg.sentinel_test_key, "before-failover"});
        if (!try_awaited_status(write_before, "OK")) {
            if (!write_before) {
                fail("Sentinel write(before) failed: " + std::string(write_before.error().message()));
            } else {
                fail("Sentinel write(before) did not return OK");
            }
            break;
        }

        if (cfg.trigger_sentinel_failover) {
            auto sentinel_ctl = RedisClientBuilder().scheduler(scheduler).build();
            auto ctl_connect = co_await sentinel_ctl.connect(cfg.sentinel_host, cfg.sentinel_port);
            if (!ctl_connect) {
                fail("Sentinel ctl connect failed: " + std::string(ctl_connect.error().message()));
                break;
            }
            auto failover_cmd = co_await sentinel_ctl.command(
                                    command_builder.command("SENTINEL",
                                                            {"FAILOVER", cfg.sentinel_master_name}))
                                    .timeout(std::chrono::seconds(5));
            if (!failover_cmd || !failover_cmd.value().has_value()) {
                fail("Sentinel FAILOVER command failed");
                break;
            }
            co_await sentinel_ctl.close();

            bool write_after_ok = false;
            for (int i = 0; i < 120; ++i) {
                auto refresh = co_await ms.refresh_from_sentinel();
                (void)refresh;
                auto write_after = co_await ms.execute("SET",
                                                       {cfg.sentinel_test_key, "after-failover"});
                if (try_awaited_status(write_after, "OK")) {
                    write_after_ok = true;
                    break;
                }
                co_await galay::kernel::sleep(std::chrono::milliseconds(500));
            }
            if (!write_after_ok) {
                fail("Sentinel auto-failover write retry exhausted");
                break;
            }
        }

        bool read_ok = false;
        for (int i = 0; i < 60; ++i) {
            auto read_after = co_await ms.execute("GET", {cfg.sentinel_test_key}, false);
            std::string read_value;
            if (try_awaited_single_string_reply(read_after, &read_value)) {
                if (read_value == "after-failover" || read_value == "before-failover") {
                    read_ok = true;
                    break;
                }
            }
            co_await galay::kernel::sleep(std::chrono::milliseconds(500));
        }
        if (!read_ok) {
            auto read_master = co_await ms.execute("GET", {cfg.sentinel_test_key}, true);
            std::string read_value;
            if (try_awaited_single_string_reply(read_master, &read_value) &&
                (read_value == "after-failover" || read_value == "before-failover")) {
                read_ok = true;
            }
        }
        if (!read_ok) {
            fail("Sentinel read after failover failed");
            break;
        }

    } while (false);

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_done = true;
    }
    g_cv.notify_one();
}

int main()
{
    const auto cfg = load_config();
    if (!cfg.enabled) {
        std::cout << "[SKIP] set GALAY_IT_ENABLE=1 to run cluster+sentinel integration test" << std::endl;
        return redis_test::kRedisTestSkippedExitCode;
    }

    std::cout << "Running real Redis Cluster + Sentinel integration test..." << std::endl;

    Runtime runtime;
    runtime.start();
    auto* scheduler = runtime.get_next_io_scheduler();
    if (!scheduler) {
        std::cerr << "[FAILED] get scheduler failed" << std::endl;
        runtime.stop();
        return 1;
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_done = false;
    }
    g_failures.store(0);

    schedule_task(scheduler, run_integration(scheduler, cfg));

    bool finished = false;
    {
        std::unique_lock<std::mutex> lock(g_mutex);
        finished = g_cv.wait_for(lock, std::chrono::seconds(180), [] { return g_done; });
    }
    runtime.stop();

    if (!finished) {
        std::cerr << "[FAILED] integration test timeout" << std::endl;
        return 1;
    }
    if (g_failures.load() != 0) {
        std::cerr << "[FAILED] integration test failure count=" << g_failures.load() << std::endl;
        return 1;
    }

    std::cout << "[PASSED] real cluster+sentinel integration test passed" << std::endl;
    return 0;
}
