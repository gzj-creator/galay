#include <galay/cpp/galay-rpc/kernel/rpc_endpoint_cache.h>

#include <iostream>
#include <string>
#include <vector>

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

RpcEndpointInfo endpoint(std::string instance_id,
                         uint16_t port,
                         uint32_t weight = 100,
                         RpcEndpointStatus status = RpcEndpointStatus::Serving)
{
    RpcEndpointInfo info;
    info.host = "127.0.0.1";
    info.port = port;
    info.service = "Echo";
    info.instance_id = std::move(instance_id);
    info.weight = weight;
    info.status = status;
    info.version = "v1";
    info.zone = "local";
    info.metadata["role"] = "test";
    return info;
}

} // namespace

int main()
{
    RpcEndpointCache cache;
    cache.apply(RpcEndpointEvent::add(endpoint("a", 7001)));
    cache.apply(RpcEndpointEvent::add(endpoint("b", 7002, 0)));
    cache.apply(RpcEndpointEvent::add(endpoint("c", 7003, 50, RpcEndpointStatus::Draining)));

    auto snapshot = cache.snapshot("Echo");
    if (auto rc = expect(snapshot.size() == 3, "snapshot did not include all endpoints")) {
        return rc;
    }
    if (auto rc = expect(cache.selectable("Echo").size() == 1,
                         "weight 0 or non-serving endpoint was selectable")) {
        return rc;
    }
    if (auto rc = expect(cache.selectable("Echo").front().instance_id == "a",
                         "wrong endpoint selected as serving")) {
        return rc;
    }

    auto previous = cache.snapshot();
    auto updated = endpoint("a", 8001, 25);
    updated.metadata["role"] = "updated";
    cache.apply(RpcEndpointEvent::update(updated));
    if (auto rc = expect(previous->by_service.at("Echo").at(0).port == 7001,
                         "old snapshot was mutated by cache update")) {
        return rc;
    }

    auto after_update = cache.snapshot("Echo");
    if (auto rc = expect(after_update.size() == 3 && after_update[0].port == 8001 &&
                             after_update[0].metadata.at("role") == "updated",
                         "endpoint update was not visible in new snapshot")) {
        return rc;
    }

    cache.apply(RpcEndpointEvent::remove("Echo", "a"));
    if (auto rc = expect(cache.snapshot("Echo").size() == 2, "endpoint remove failed")) {
        return rc;
    }
    if (auto rc = expect(cache.selectable("Echo").empty(),
                         "removed endpoint was still selectable")) {
        return rc;
    }

    std::cout << "RPC endpoint cache PASS\n";
    return 0;
}
