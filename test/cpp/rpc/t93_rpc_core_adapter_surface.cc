#include <galay/cpp/galay-rpc/kernel/rpc_discovery.h>

#ifdef GALAY_RPC_HAS_ETCD
#error "galay::rpc must not expose the optional etcd adapter feature macro"
#endif

int main()
{
    galay::rpc::RpcEndpointInfo endpoint;
    endpoint.service = "core";
    endpoint.instance_id = "local";
    endpoint.host = "127.0.0.1";
    endpoint.port = 50051;

    return endpoint.service == "core" ? 0 : 1;
}
