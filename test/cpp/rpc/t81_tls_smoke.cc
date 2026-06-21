#include <galay/cpp/galay-rpc/kernel/rpc_tls.h>

#include <iostream>

int main()
{
    if (!galay::rpc::rpcTlsCompiled()) {
        std::cout << "RPC TLS smoke SKIP: TLS support is not compiled into rpc target\n";
        return 0;
    }
    galay::rpc::RpcTlsConfig config;
    config.enabled = true;
    std::cout << "RPC TLS smoke PASS\n";
    return 0;
}
