#include <galay/cpp/galay-rpc/discovery/etcd_service_registry.h>

#ifndef GALAY_RPC_HAS_ETCD
#error "galay::rpc must expose the etcd adapter feature macro when GALAY_RPC_ENABLE_ETCD is enabled"
#endif

int main()
{
    galay::rpc::RpcEtcdRegistryConfig config;
    config.endpoint = "http://127.0.0.1:2379";
    config.prefix = "/galay/test";

    galay::rpc::EtcdServiceRegistry registry(config);
    (void)registry;

    galay::rpc::FakeEtcdServiceRegistry fake(config.prefix);
    galay::rpc::RpcEndpointInfo endpoint;
    endpoint.service = "adapter";
    endpoint.instance_id = "local";
    endpoint.host = "127.0.0.1";
    endpoint.port = 50051;

    return fake.registerEndpoint(endpoint).has_value() ? 0 : 1;
}
