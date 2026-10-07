import galay.rpc;

#include <iostream>

int main()
{
    if (!galay::rpc::rpc_tls_compiled()) {
        std::cout << "RPC TLS import example skipped: TLS support is optional\n";
        return 0;
    }
    std::cout << "RPC TLS import example ready\n";
    return 0;
}
