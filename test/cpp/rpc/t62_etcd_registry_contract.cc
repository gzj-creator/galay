#include <galay/cpp/galay-rpc/discovery/etcd_service_registry.h>

#include <cstdlib>
#include <iostream>
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

RpcEndpointInfo endpoint()
{
    RpcEndpointInfo info;
    info.host = "127.0.0.1";
    info.port = 7010;
    info.service = "Echo";
    info.instance_id = "fake-1";
    info.weight = 100;
    info.status = RpcEndpointStatus::Serving;
    return info;
}

} // namespace

int main()
{
    RpcEndpointCache cache;
    FakeEtcdServiceRegistry fake("/galay/rpc");
    if (auto rc = expect(fake.registerEndpoint(endpoint()).has_value(),
                         "fake registry register failed")) {
        return rc;
    }
    auto discovered = fake.discover("Echo");
    if (auto rc = expect(discovered.has_value() && discovered->size() == 1,
                         "fake registry discover contract failed")) {
        return rc;
    }

    bool watched = false;
    auto watch = fake.watch("Echo", cache, [&] { watched = true; });
    if (auto rc = expect(watch.has_value(), "fake registry watch failed")) {
        return rc;
    }
    auto updated = endpoint();
    updated.port = 7011;
    if (auto rc = expect(fake.registerEndpoint(updated).has_value(),
                         "fake registry update failed")) {
        return rc;
    }
    if (auto rc = expect(watched, "fake registry did not notify watcher")) {
        return rc;
    }
    if (auto rc = expect(cache.selectable("Echo").size() == 1 &&
                             cache.selectable("Echo").front().port == 7011,
                         "fake registry did not update endpoint cache")) {
        return rc;
    }
    if (auto rc = expect(fake.deregisterEndpoint("Echo", "fake-1").has_value(),
                         "fake registry deregister failed")) {
        return rc;
    }
    if (auto rc = expect(cache.snapshot("Echo").empty(),
                         "fake registry did not remove endpoint from cache")) {
        return rc;
    }

    const char* endpoint_env = std::getenv("GALAY_ETCD_ENDPOINT");
    if (endpoint_env == nullptr || endpoint_env[0] == '\0') {
        std::cout << "RPC etcd real integration SKIP: GALAY_ETCD_ENDPOINT is not set\n";
        std::cout << "RPC etcd registry contract PASS\n";
        return 0;
    }

    EtcdServiceRegistry registry(RpcEtcdRegistryConfig{.endpoint = endpoint_env,
                                                       .prefix = "/galay/rpc/test"});
    auto real_result = registry.integrationAvailable();
    if (auto rc = expect(real_result.has_value(), "real etcd adapter reported unavailable")) {
        return rc;
    }

    std::cout << "RPC etcd registry contract PASS\n";
    return 0;
}
