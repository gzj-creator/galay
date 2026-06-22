#include <galay/cpp/galay-rpc/kernel/rpc_discovery.h>

#include <iostream>

using namespace galay::rpc;

namespace {

int expect(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << "\n";
        return 1;
    }
    return 0;
}

ServiceEndpoint endpoint(const char* host, uint16_t port, const char* instance, uint32_t weight = 100)
{
    ServiceEndpoint ep;
    ep.host = host;
    ep.port = port;
    ep.service_name = "EchoService";
    ep.instance_id = instance;
    ep.weight = weight;
    return ep;
}

} // namespace

int main()
{
    LocalServiceRegistry registry;
    registry.registerService(endpoint("127.0.0.1", 7001, "a"));
    registry.registerService(endpoint("127.0.0.1", 7002, "b"));

    ServiceDiscoveryClient<LocalServiceRegistry, RoundRobinSelector> client(registry);
    auto first = client.getServiceEndpoint("EchoService");
    auto second = client.getServiceEndpoint("EchoService");
    auto third = client.getServiceEndpoint("EchoService");
    if (auto rc = expect(first.has_value() &&
                             second.has_value() &&
                             third.has_value() &&
                             first->instance_id == "a" &&
                             second->instance_id == "b" &&
                             third->instance_id == "a",
                         "round-robin selector did not preserve position across discovery calls")) {
        return rc;
    }

    ServiceDiscoveryClient<LocalServiceRegistry, WeightedRoundRobinSelector> weighted_rr(registry);
    auto weighted_rr_selected = weighted_rr.getServiceEndpoint("EchoService");
    if (auto rc = expect(weighted_rr_selected.has_value(),
                         "weighted round-robin selector was not constructible through discovery client")) {
        return rc;
    }

    ServiceDiscoveryClient<LocalServiceRegistry, WeightedRandomSelector> weighted_random(registry);
    auto weighted_random_selected = weighted_random.getServiceEndpoint("EchoService");
    if (auto rc = expect(weighted_random_selected.has_value(),
                         "weighted random selector was not constructible through discovery client")) {
        return rc;
    }

    std::cout << "RPC discovery selector PASS\n";
    return 0;
}
