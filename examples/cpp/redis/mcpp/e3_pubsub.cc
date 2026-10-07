#include "common/config.h"
#include <galay/cpp/galay-kernel/core/runtime.h>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>

import galay.redis;

using namespace galay::kernel;
using namespace galay::redis;

namespace {

struct DemoState {
    std::mutex mutex;
    std::condition_variable cv;
    bool done{false};
    int code{1};
};

void finish_demo(DemoState& state, int code)
{
    std::lock_guard<std::mutex> lock(state.mutex);
    state.done = true;
    state.code = code;
    state.cv.notify_one();
}

std::optional<int> parse_port(const char* text)
{
    if (text == nullptr) return std::nullopt;
    try {
        const int value = std::stoi(text);
        if (value <= 0 || value > 65535) return std::nullopt;
        return value;
    } catch (...) {
        return std::nullopt;
    }
}

bool read_command_single_string(const RedisCommandResult& result, std::string& out)
{
    if (!result || result.value().empty()) {
        return false;
    }
    const auto& first = result.value().front();
    if (first.is_string()) {
        out = first.to_string();
        return true;
    }
    if (first.is_status()) {
        out = first.to_status();
        return true;
    }
    return false;
}

bool read_awaited_command_single_string(auto&& result, std::string& out)
{
    return result && result.value() && read_command_single_string(result.value(), out);
}

Task<bool> run_pub_sub_demo(IOScheduler* scheduler, std::string host, int port)
{
    auto subscriber = RedisClientBuilder().scheduler(scheduler).build();
    auto publisher = RedisClientBuilder().scheduler(scheduler).build();
    RedisCommandBuilder command_builder;

    const std::string channel = galay::redis::example::kDefaultPubSubChannel;
    const std::string message = galay::redis::example::kDefaultPubSubMessage;

    auto subscriber_connect = co_await subscriber.connect(host, port).timeout(std::chrono::seconds(5));
    if (!subscriber_connect) {
        std::cerr << "Subscriber connect failed: " << subscriber_connect.error().message() << std::endl;
        co_return false;
    }

    auto publisher_connect = co_await publisher.connect(host, port).timeout(std::chrono::seconds(5));
    if (!publisher_connect) {
        std::cerr << "Publisher connect failed: " << publisher_connect.error().message() << std::endl;
        (void)co_await subscriber.close();
        co_return false;
    }

    auto subscribe_result = co_await subscriber.command(command_builder.subscribe(channel))
                                .timeout(std::chrono::seconds(5));
    if (!subscribe_result || !subscribe_result.value()) {
        std::cerr << "SUBSCRIBE failed" << std::endl;
        (void)co_await subscriber.close();
        (void)co_await publisher.close();
        co_return false;
    }

    auto publish_result = co_await publisher.command(command_builder.publish(channel, message))
                              .timeout(std::chrono::seconds(5));
    if (!publish_result || !publish_result.value()) {
        std::cerr << "PUBLISH failed" << std::endl;
        (void)co_await subscriber.command(command_builder.unsubscribe(channel));
        (void)co_await subscriber.close();
        (void)co_await publisher.close();
        co_return false;
    }

    auto recv_result = co_await subscriber.receive(1).timeout(std::chrono::seconds(5));
    if (!recv_result || !recv_result.value() || recv_result.value()->empty()) {
        std::cerr << "Receive pubsub message failed" << std::endl;
        (void)co_await subscriber.command(command_builder.unsubscribe(channel));
        (void)co_await subscriber.close();
        (void)co_await publisher.close();
        co_return false;
    }

    const auto msg_array = recv_result.value()->front().to_array();
    if (msg_array.size() < 3 || !msg_array[2].is_string() || msg_array[2].to_string() != message) {
        std::cerr << "PubSub payload mismatch" << std::endl;
        (void)co_await subscriber.command(command_builder.unsubscribe(channel));
        (void)co_await subscriber.close();
        (void)co_await publisher.close();
        co_return false;
    }
    std::cout << "E3 import pubsub received: " << msg_array[2].to_string() << std::endl;

    (void)co_await subscriber.command(command_builder.unsubscribe(channel));
    (void)co_await subscriber.close();
    (void)co_await publisher.close();

    co_return true;
}

Task<bool> run_master_slave_demo(IOScheduler* scheduler, std::string host, int port)
{
    auto ms_client = RedisMasterSlaveClientBuilder().scheduler(scheduler).build();
    RedisNodeAddress node_addr;
    node_addr.host = host;
    node_addr.port = port;
    ms_client.set_auto_retry_attempts(3);

    auto master_connect = co_await ms_client.connect_master(node_addr).timeout(std::chrono::seconds(5));
    if (!master_connect) {
        std::cerr << "Master connect failed: " << master_connect.error().message() << std::endl;
        co_return false;
    }

    auto replica_connect = co_await ms_client.add_replica(node_addr).timeout(std::chrono::seconds(5));
    if (!replica_connect) {
        std::cerr << "Replica connect failed: " << replica_connect.error().message() << std::endl;
        (void)co_await ms_client.master().close();
        co_return false;
    }

    const std::string ms_key = "example:ms:key";
    const std::string ms_value = "ms-ok";
    auto ms_set = co_await ms_client.execute("SET", {ms_key, ms_value});
    if (!ms_set) {
        std::cerr << "Master write failed: " << ms_set.error().message() << std::endl;
        if (auto repl = ms_client.replica(0); repl.has_value()) {
            (void)co_await repl->get().close();
        }
        (void)co_await ms_client.master().close();
        co_return false;
    }

    auto ms_get = co_await ms_client.execute("GET", {ms_key}, true);
    std::string ms_read_value;
    if (!read_awaited_command_single_string(ms_get, ms_read_value) || ms_read_value != ms_value) {
        std::cerr << "Replica read value mismatch, got: " << ms_read_value << std::endl;
        if (auto repl = ms_client.replica(0); repl.has_value()) {
            (void)co_await repl->get().close();
        }
        (void)co_await ms_client.master().close();
        co_return false;
    }
    std::cout << "E3 import master-slave read: " << ms_read_value << std::endl;

    if (auto repl = ms_client.replica(0); repl.has_value()) {
        (void)co_await repl->get().close();
    }
    (void)co_await ms_client.master().close();

    co_return true;
}

Task<bool> run_cluster_demo(IOScheduler* scheduler, std::string host, int port)
{
    auto cluster_client = RedisClusterClientBuilder().scheduler(scheduler).build();
    RedisClusterNodeAddress cluster_node;
    cluster_node.host = host;
    cluster_node.port = port;
    cluster_node.slot_start = 0;
    cluster_node.slot_end = 16383;

    auto cluster_connect = co_await cluster_client.add_node(cluster_node).timeout(std::chrono::seconds(5));
    if (!cluster_connect) {
        std::cerr << "Cluster node connect failed: " << cluster_connect.error().message() << std::endl;
        co_return false;
    }

    const std::string cluster_key = galay::redis::example::kDefaultTopologyKey;
    const std::string cluster_value = galay::redis::example::kDefaultTopologyValue;

    auto cluster_set = co_await cluster_client.execute("SET", {cluster_key, cluster_value}, cluster_key);
    if (!cluster_set || !cluster_set.value() || cluster_set.value().value().empty()) {
        std::cerr << "Cluster SET failed" << std::endl;
        if (auto node = cluster_client.node(0); node.has_value()) {
            (void)co_await node->get().close();
        }
        co_return false;
    }

    auto cluster_get = co_await cluster_client.execute("GET", {cluster_key}, cluster_key);
    std::string cluster_read_value;
    if (!read_awaited_command_single_string(cluster_get, cluster_read_value) || cluster_read_value != cluster_value) {
        std::cerr << "Cluster GET value mismatch" << std::endl;
        if (auto node = cluster_client.node(0); node.has_value()) {
            (void)co_await node->get().close();
        }
        co_return false;
    }
    std::cout << "E3 import cluster read: " << cluster_read_value << std::endl;

    if (auto node = cluster_client.node(0); node.has_value()) {
        (void)co_await node->get().close();
    }

    co_return true;
}

Task<void> run_demo(IOScheduler* scheduler, DemoState* state, std::string host, int port)
{
    auto pubsub_ok = co_await run_pub_sub_demo(scheduler, host, port);
    if (!pubsub_ok) {
        finish_demo(*state, 1);
        co_return;
    }

    auto master_slave_ok = co_await run_master_slave_demo(scheduler, host, port);
    if (!master_slave_ok) {
        finish_demo(*state, 1);
        co_return;
    }

    auto cluster_ok = co_await run_cluster_demo(scheduler, host, port);
    if (!cluster_ok) {
        finish_demo(*state, 1);
        co_return;
    }

    finish_demo(*state, 0);
}

}  // namespace

int main(int argc, char* argv[])
{
    std::string host = galay::redis::example::kDefaultRedisHost;
    int port = galay::redis::example::kDefaultRedisPort;

    if (argc > 1) host = argv[1];
    if (argc > 2) {
        auto parsed_port = parse_port(argv[2]);
        if (!parsed_port) {
            std::cerr << "Invalid port: " << argv[2] << std::endl;
            std::cerr << "Usage: " << argv[0] << " [host] [port]" << std::endl;
            return 1;
        }
        port = *parsed_port;
    }

    Runtime runtime;
    runtime.start();

    auto* scheduler = runtime.get_next_io_scheduler();
    if (!scheduler) {
        std::cerr << "Failed to get IO scheduler" << std::endl;
        runtime.stop();
        return 1;
    }

    DemoState state;
    schedule_task(scheduler, run_demo(scheduler, &state, host, port));

    std::unique_lock<std::mutex> lock(state.mutex);
    const bool finished = state.cv.wait_for(lock, std::chrono::seconds(30), [&]() {
        return state.done;
    });

    runtime.stop();

    if (!finished) {
        std::cerr << "Demo timeout after 30s" << std::endl;
        return 1;
    }
    return state.code;
}
