import galay.rpc;

#include <iostream>

using namespace galay::rpc;

int main()
{
    RpcEndpointInfo endpoint;
    endpoint.host = "127.0.0.1";
    endpoint.port = 50052;
    endpoint.service = "ExampleEcho";
    endpoint.instance_id = "example-import-1";
    endpoint.version = "v1";
    endpoint.zone = "local";

    RpcEndpointCache cache;
    FakeEtcdServiceRegistry registry("/galay/rpc/examples");
    auto registered = registry.registerEndpoint(endpoint);
    if (!registered.has_value()) {
        std::cerr << registered.error().message() << "\n";
        return 1;
    }
    auto watched = registry.watch("ExampleEcho", cache);
    if (!watched.has_value()) {
        std::cerr << watched.error().message() << "\n";
        return 1;
    }

    auto endpoints = cache.selectable("ExampleEcho");
    if (endpoints.empty()) {
        std::cerr << "no selectable endpoint\n";
        return 1;
    }

    std::cout << endpoints.front().address() << "\n";
    return 0;
}
