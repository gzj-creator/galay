#include "test_common.hpp"

void test_consistent_hash() {
    std::cout << "=== Testing ConsistentHash ===" << std::endl;

    ConsistentHash hash(100);

    hash.addNode({"node1", "192.168.1.1:8080", 1});
    hash.addNode({"node2", "192.168.1.2:8080", 1});
    hash.addNode({"node3", "192.168.1.3:8080", 2}); // Weight 2

    assert(hash.nodeCount() == 3);
    assert(hash.virtualNodeCount() == 400); // 100 + 100 + 200

    // Get node for key
    auto node = hash.getNode("test_key");
    assert(node.has_value());

    // Same key should always return same node
    auto node2 = hash.getNode("test_key");
    assert(node2.has_value());
    assert(node->id == node2->id);

    // Get multiple nodes for replication
    auto nodes = hash.getNodes("test_key", 2);
    assert(nodes.size() == 2);
    assert(nodes[0].id != nodes[1].id);

    // Remove node
    hash.removeNode("node1");
    assert(hash.nodeCount() == 2);

    std::cout << "ConsistentHash tests passed!" << std::endl;
}

// ==================== TrieTree Tests ====================

void test_load_balancer() {
    std::cout << "=== Testing LoadBalancer ===" << std::endl;

    std::vector<std::string> nodes = {"node1", "node2", "node3"};

    // Round Robin
    RoundRobinLoadBalancer<std::string> rrBalancer(nodes);
    assert(rrBalancer.size() == 3);

    auto selected1 = rrBalancer.select();
    auto selected2 = rrBalancer.select();
    auto selected3 = rrBalancer.select();
    auto selected4 = rrBalancer.select();

    assert(selected1.has_value() && selected2.has_value() && selected3.has_value() && selected4.has_value());
    assert(selected1.value() == "node1" && selected2.value() == "node2" && selected3.value() == "node3");
    assert(selected4.value() == "node1"); // Round robin

    rrBalancer.append("node4");
    assert(rrBalancer.size() == 4);

    // Weighted Round Robin
    std::vector<uint32_t> weights = {3, 2, 1};
    WeightRoundRobinLoadBalancer<std::string> wrrBalancer(nodes, weights);
    assert(wrrBalancer.size() == 3);

    // Test weighted selection (should prefer higher weight nodes)
    std::map<std::string, int> counts;
    for (int i = 0; i < 12; ++i) { // 3+2+1=6, test 2 rounds
        auto selected = wrrBalancer.select();
        assert(selected.has_value());
        counts[selected.value()]++;
    }

    // node1 (weight 3) should be selected more than node3 (weight 1)
    assert(counts["node1"] > counts["node3"]);

    // Random Load Balancer
    RandomLoadBalancer<std::string> randomBalancer(nodes);
    assert(randomBalancer.size() == 3);

    auto randomSelected = randomBalancer.select();
    assert(randomSelected.has_value());
    assert(std::find(nodes.begin(), nodes.end(), randomSelected.value()) != nodes.end());

    randomBalancer.append("node5");
    assert(randomBalancer.size() == 4);

    // Weighted Random Load Balancer
    WeightedRandomLoadBalancer<std::string> weightedRandomBalancer(nodes, weights);
    assert(weightedRandomBalancer.size() == 3);

    // Test weighted random selection
    counts.clear();
    for (int i = 0; i < 1000; ++i) {
        auto selected = weightedRandomBalancer.select();
        assert(selected.has_value());
        counts[selected.value()]++;
    }

    // node1 should be selected roughly 3 times more than node3
    assert(counts["node1"] > counts["node2"] && counts["node2"] > counts["node3"]);

    // Edge cases
    std::vector<std::string> emptyNodes;
    RoundRobinLoadBalancer<std::string> emptyRR(emptyNodes);
    assert(emptyRR.size() == 0);
    assert(!emptyRR.select().has_value());

    RandomLoadBalancer<std::string> emptyRandom(emptyNodes);
    assert(emptyRandom.size() == 0);
    assert(!emptyRandom.select().has_value());

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
