import galay.rpc;

#include <iostream>

int main()
{
    if (!galay::rpc::rpcTlsCompiled()) {
        std::cout << "RPC TLS import example skipped: TLS support is optional\n";
        return 0;
    }
    std::cout << "RPC TLS import example ready\n";
    return 0;
}
