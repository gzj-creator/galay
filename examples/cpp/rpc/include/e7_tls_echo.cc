#include <galay/cpp/galay-rpc/kernel/rpc_tls.h>

#include <iostream>

int main()
{
    if (!galay::rpc::rpcTlsCompiled()) {
        std::cout << "RPC TLS example skipped: TLS support is optional\n";
        return 0;
    }
    std::cout << "RPC TLS example ready\n";
    return 0;
}
