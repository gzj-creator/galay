import galay.rpc;

int main()
{
    galay::rpc::RpcRequest request(1, "ModuleSmoke", "ping");
    return request.serviceName() == "ModuleSmoke" ? 0 : 1;
}
