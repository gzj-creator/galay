#include <galay-kernel/core/runtime.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>

#include "galay-redis/async/redis_client.h"
#include "galay-redis/async/topology_client.h"

using namespace galay::kernel;
using namespace galay::redis;

namespace
{
    std::mutex g_mutex;
    std::condition_variable g_cv;
    bool g_done = false;
    std::atomic<int> g_failures{0};

    void fail(const std::string& msg)
    {
        ++g_failures;
        std::cerr << "[FAILED] " << msg << std::endl;
    }

    bool expectSingleStringReply(const std::expected<std::optional<std::vector<RedisValue>>, RedisError>& result,
                                 std::string* out)
    {
        if (!result) {
            fail("Command failed: " + std::string(result.error().message()));
            return false;
        }
        if (!result.value().has_value() || result.value()->empty()) {
            fail("Reply is empty");
            return false;
        }
        const auto& first = result.value()->front();
        if (!first.isString() && !first.isStatus()) {
            fail("Reply is not string/status");
            return false;
        }
        *out = first.isString() ? first.toString() : first.toStatus();
        return true;
    }

    bool expectSingleStringResult(const RedisCommandResult& result, std::string* out)
    {
        if (!result) {
            fail("Command failed: " + std::string(result.error().message()));
            return false;
        }
        if (result.value().empty()) {
            fail("Reply is empty");
            return false;
        }
        const auto& first = result.value().front();
        if (!first.isString() && !first.isStatus()) {
            fail("Reply is not string/status");
            return false;
        }
        *out = first.isString() ? first.toString() : first.toStatus();
        return true;
    }

    bool expectAwaitedSingleStringResult(auto&& result, std::string* out)
    {
        if (!result) {
            fail("Command failed: " + std::string(result.error().message()));
            return false;
        }
        if (!result.value()) {
            fail("Command failed: " + std::string(result.value().error().message()));
            return false;
        }
        return expectSingleStringResult(result.value(), out);
    }
}

Task<void> runTopologyAndPubSubTests(IOScheduler* scheduler)
{
    auto subscriber = RedisClientBuilder().scheduler(scheduler).build();
    auto publisher = RedisClientBuilder().scheduler(scheduler).build();
    RedisCommandBuilder command_builder;
    const std::string channel = "galay:test:pubsub";

    do {
        auto subscriber_connect = co_await subscriber.connect("127.0.0.1", 6379);
        if (!subscriber_connect) {
            fail("Subscriber connect failed: " + std::string(subscriber_connect.error().message()));
            break;
        }

        auto publisher_connect = co_await publisher.connect("127.0.0.1", 6379);
        if (!publisher_connect) {
            fail("Publisher connect failed: " + std::string(publisher_connect.error().message()));
            break;
        }

        const std::string message = "hello-pubsub";
        auto subscribe_result = co_await subscriber.command(command_builder.subscribe(channel))
                                    .timeout(std::chrono::seconds(5));
        if (!subscribe_result || !subscribe_result.value() || subscribe_result.value()->empty()) {
            fail("SUBSCRIBE failed");
            break;
        }

        auto publish_result = co_await publisher.command(command_builder.publish(channel, message))
                                  .timeout(std::chrono::seconds(5));
        if (!publish_result || !publish_result.value() || publish_result.value()->empty()) {
            fail("PUBLISH failed");
            break;
        }

        auto pubsub_message = co_await subscriber.receive(1).timeout(std::chrono::seconds(5));
        if (!pubsub_message || !pubsub_message.value() || pubsub_message.value()->empty()) {
            fail("Receive pubsub message failed");
            break;
        }

        const auto msg_array = pubsub_message.value()->front().toArray();
        if (msg_array.size() < 3) {
            fail("Pubsub message payload format invalid");
            break;
        }

        if (!msg_array[0].isString() || msg_array[0].toString() != "message") {
            fail("Pubsub message type mismatch");
            break;
        }
        if (!msg_array[1].isString() || msg_array[1].toString() != channel) {
            fail("Pubsub channel mismatch");
            break;
        }
        if (!msg_array[2].isString() || msg_array[2].toString() != message) {
            fail("Pubsub payload mismatch");
            break;
        }

        {
            auto ms_client = RedisMasterSlaveClientBuilder().scheduler(scheduler).build();
            RedisNodeAddress node_addr;
            node_addr.host = "127.0.0.1";
            node_addr.port = 6379;

            auto master_connect = co_await ms_client.connectMaster(node_addr);
            if (!master_connect) {
                fail("Master connect failed: " + std::string(master_connect.error().message()));
                break;
            }

            auto replica_connect = co_await ms_client.addReplica(node_addr);
            if (!replica_connect) {
                fail("Replica connect failed: " + std::string(replica_connect.error().message()));
                break;
            }

            const std::string key = "galay:test:ms:key";
            const std::string value = "ms-value";

            auto write_result = co_await ms_client.execute("SET", {key, value}, false, false);
            if (!write_result || !write_result.value() || write_result.value().value().empty()) {
                fail("Master write failed");
                break;
            }

            auto read_result = co_await ms_client.execute("GET", {key}, true, false);
            std::string read_value;
            if (!expectAwaitedSingleStringResult(read_result, &read_value)) {
                break;
            }
            if (read_value != value) {
                fail("Replica read value mismatch");
                break;
            }

            if (auto repl = ms_client.replica(0); repl.has_value()) {
                co_await repl->get().close();
            }
            co_await ms_client.master().close();
        }

        {
            auto ms_auto = RedisMasterSlaveClientBuilder().scheduler(scheduler).build();
            ms_auto.setAutoRetryAttempts(3);
            RedisNodeAddress node_addr;
            node_addr.host = "127.0.0.1";
            node_addr.port = 6379;

            auto master_connect = co_await ms_auto.connectMaster(node_addr);
            if (!master_connect) {
                fail("Master connect (auto) failed: " + std::string(master_connect.error().message()));
                break;
            }

            auto write_auto = co_await ms_auto.execute("SET", {"galay:test:auto:ms", "ok"});
            if (!write_auto) {
                fail("execute(write) failed: " + std::string(write_auto.error().message()));
                break;
            }

            auto read_auto = co_await ms_auto.execute("GET", {"galay:test:auto:ms"}, true);
            std::string read_auto_value;
            if (!expectAwaitedSingleStringResult(read_auto, &read_auto_value)) {
                break;
            }
            if (read_auto_value != "ok") {
                fail("execute(read) value mismatch");
                break;
            }

            // 在非 Sentinel 环境下，refreshFromSentinel 允许失败，但不能崩溃
            auto sentinel_refresh = co_await ms_auto.refreshFromSentinel();
            if (sentinel_refresh && sentinel_refresh.value() && sentinel_refresh.value().value().empty()) {
                fail("refreshFromSentinel unexpected empty success");
                break;
            }

            co_await ms_auto.master().close();
        }

        {
            auto cluster_client = RedisClusterClientBuilder().scheduler(scheduler).build();

            RedisClusterNodeAddress node1;
            node1.host = "127.0.0.1";
            node1.port = 6379;
            node1.slot_start = 0;
            node1.slot_end = 8191;

            RedisClusterNodeAddress node2;
            node2.host = "127.0.0.1";
            node2.port = 6379;
            node2.slot_start = 8192;
            node2.slot_end = 16383;

            auto node1_connect = co_await cluster_client.addNode(node1);
            if (!node1_connect) {
                fail("Cluster node1 connect failed: " + std::string(node1_connect.error().message()));
                break;
            }

            auto node2_connect = co_await cluster_client.addNode(node2);
            if (!node2_connect) {
                fail("Cluster node2 connect failed: " + std::string(node2_connect.error().message()));
                break;
            }

            const auto slot_a = cluster_client.keySlot("{user100}:name");
            const auto slot_b = cluster_client.keySlot("{user100}:email");
            if (slot_a != slot_b) {
                fail("Cluster hashtag slot mismatch");
                break;
            }

            const std::string cluster_key = "{user100}:name";
            const std::string cluster_value = "cluster-ok";

            auto cluster_set = co_await cluster_client.execute("SET",
                                                               {cluster_key, cluster_value},
                                                               cluster_key,
                                                               false);
            if (!cluster_set || !cluster_set.value() || cluster_set.value().value().empty()) {
                fail("Cluster SET failed");
                break;
            }

            auto cluster_get = co_await cluster_client.execute("GET",
                                                               {cluster_key},
                                                               cluster_key,
                                                               false);
            std::string get_value;
            if (!expectAwaitedSingleStringResult(cluster_get, &get_value)) {
                break;
            }
            if (get_value != cluster_value) {
                fail("Cluster GET value mismatch");
                break;
            }

            auto cluster_auto_set = co_await cluster_client.execute("SET",
                                                                    {"{u200}:name", "auto-cluster"},
                                                                    "{u200}:name",
                                                                    true);
            if (!cluster_auto_set || !cluster_auto_set.value() || cluster_auto_set.value().value().empty()) {
                fail("Cluster auto SET failed");
                break;
            }

            auto cluster_auto_get = co_await cluster_client.execute("GET",
                                                                    {"{u200}:name"},
                                                                    "{u200}:name",
                                                                    true);
            std::string auto_get_value;
            if (!expectAwaitedSingleStringResult(cluster_auto_get, &auto_get_value)) {
                break;
            }
            if (auto_get_value != "auto-cluster") {
                fail("Cluster auto GET value mismatch");
                break;
            }

            for (size_t i = 0; i < cluster_client.nodeCount(); ++i) {
                auto cluster_node = cluster_client.node(i);
                if (cluster_node.has_value()) {
                    co_await cluster_node->get().close();
                }
            }
        }
    } while (false);

    co_await subscriber.command(command_builder.unsubscribe(channel));
    co_await subscriber.close();
    co_await publisher.close();

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_done = true;
    }
    g_cv.notify_one();
}

int main()
{
    std::cout << "Running topology and pubsub tests..." << std::endl;

    Runtime runtime;
    runtime.start();

    auto* scheduler = runtime.getNextIOScheduler();
    if (!scheduler) {
        std::cerr << "[FAILED] Failed to get scheduler" << std::endl;
        runtime.stop();
        return 1;
    }

    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_done = false;
    }

    scheduleTask(scheduler, runTopologyAndPubSubTests(scheduler));

    bool finished = false;
    {
        std::unique_lock<std::mutex> lock(g_mutex);
        finished = g_cv.wait_for(lock, std::chrono::seconds(30), [] { return g_done; });
    }

    runtime.stop();

    if (!finished) {
        std::cerr << "[FAILED] topology and pubsub tests timeout" << std::endl;
        return 1;
    }

    if (g_failures.load() == 0) {
        std::cout << "[PASSED] topology and pubsub tests passed" << std::endl;
        return 0;
    }

    std::cerr << "[FAILED] topology and pubsub tests failed, count=" << g_failures.load() << std::endl;
    return 1;
}
