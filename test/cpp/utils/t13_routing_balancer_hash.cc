#include "test_common.hpp"

void test_consistent_hash() {
    std::cout << "=== Testing ConsistentHash ===" << std::endl;

    ConsistentHash hash(100);

    hash.add_node({"node1", "192.168.1.1:8080", 1});
    hash.add_node({"node2", "192.168.1.2:8080", 1});
    hash.add_node({"node3", "192.168.1.3:8080", 2}); // Weight 2

    assert(hash.node_count() == 3);
    assert(hash.virtual_node_count() == 400); // 100 + 100 + 200

    // Get node for key
    auto node = hash.get_node("test_key");
    assert(node.has_value());

    // Same key should always return same node
    auto node2 = hash.get_node("test_key");
    assert(node2.has_value());
    assert(node->id == node2->id);

    // Get multiple nodes for replication
    auto nodes = hash.get_nodes("test_key", 2);
    assert(nodes.size() == 2);
    assert(nodes[0].id != nodes[1].id);

    // Remove node
    hash.remove_node("node1");
    assert(hash.node_count() == 2);

    std::cout << "ConsistentHash tests passed!" << std::endl;
}

// ==================== TrieTree Tests ====================

void test_load_balancer() {
    std::cout << "=== Testing LoadBalancer ===" << std::endl;

    std::vector<std::string> nodes = {"node1", "node2", "node3"};

    // Round Robin
    RoundRobinLoadBalancer<std::string> rr_balancer(nodes);
    assert(rr_balancer.size() == 3);

    auto selected1 = rr_balancer.select();
    auto selected2 = rr_balancer.select();
    auto selected3 = rr_balancer.select();
    auto selected4 = rr_balancer.select();

    assert(selected1.has_value() && selected2.has_value() && selected3.has_value() && selected4.has_value());
    assert(selected1.value() == "node1" && selected2.value() == "node2" && selected3.value() == "node3");
    assert(selected4.value() == "node1"); // Round robin

    rr_balancer.append("node4");
    assert(rr_balancer.size() == 4);

    // Weighted Round Robin
    std::vector<uint32_t> weights = {3, 2, 1};
    WeightRoundRobinLoadBalancer<std::string> wrr_balancer(nodes, weights);
    assert(wrr_balancer.size() == 3);

    // Test weighted selection (should prefer higher weight nodes)
    std::map<std::string, int> counts;
    for (int i = 0; i < 12; ++i) { // 3+2+1=6, test 2 rounds
        auto selected = wrr_balancer.select();
        assert(selected.has_value());
        counts[selected.value()]++;
    }

    // node1 (weight 3) should be selected more than node3 (weight 1)
    assert(counts["node1"] > counts["node3"]);

    // Random Load Balancer
    RandomLoadBalancer<std::string> random_balancer(nodes);
    assert(random_balancer.size() == 3);

    auto randomSelected = random_balancer.select();
    assert(randomSelected.has_value());
    assert(std::find(nodes.begin(), nodes.end(), randomSelected.value()) != nodes.end());

    random_balancer.append("node5");
    assert(random_balancer.size() == 4);

    // Weighted Random Load Balancer
    WeightedRandomLoadBalancer<std::string> weighted_random_balancer(nodes, weights);
    assert(weighted_random_balancer.size() == 3);

    // Test weighted random selection
    counts.clear();
    for (int i = 0; i < 1000; ++i) {
        auto selected = weighted_random_balancer.select();
        assert(selected.has_value());
        counts[selected.value()]++;
    }

    // node1 should be selected roughly 3 times more than node3
    assert(counts["node1"] > counts["node2"] && counts["node2"] > counts["node3"]);

    // Edge cases
    std::vector<std::string> emptyNodes;
    RoundRobinLoadBalancer<std::string> empty_rr(emptyNodes);
    assert(empty_rr.size() == 0);
    assert(!empty_rr.select().has_value());

    RandomLoadBalancer<std::string> empty_random(emptyNodes);
    assert(empty_random.size() == 0);
    assert(!empty_random.select().has_value());

    std::cout << "LoadBalancer tests passed!" << std::endl;
}

// ==================== LruCache Tests ====================

int main() {
    std::cout << "\n=== routing_test ===" << std::endl;
    try {
        test_consistent_hash();
        test_load_balancer();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Test failed with exception: " << e.what() << std::endl;
        return 1;
    }
}
