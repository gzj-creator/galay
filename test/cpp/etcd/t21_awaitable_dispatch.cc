#include <galay/cpp/galay-etcd/async/client.h>
#include <galay/cpp/galay-etcd/details/awaitable.h>

#include <iostream>
#include <vector>

using namespace galay::etcd;

template<typename Awaitable>
bool check_disconnected(Awaitable awaitable, const char* operation) {
    if (!awaitable.await_ready()) {
        std::cerr << operation << " must complete synchronously without a connection\n";
        return false;
    }
    if (awaitable.await_resume().has_value()) {
        std::cerr << operation << " must report a disconnected error\n";
        return false;
    }
    return true;
}

int main() {
    auto client = AsyncEtcdClientBuilder().build();
    bool ok = check_disconnected(client.put("key", "value"), "put");
    ok = check_disconnected(client.get("key"), "get") && ok;
    ok = check_disconnected(client.del("key"), "delete") && ok;
    ok = check_disconnected(client.grant_lease(10), "grant lease") && ok;
    ok = check_disconnected(client.keep_alive_once(1), "keep alive") && ok;
    std::vector<PipelineOp> operations;
    operations.push_back(PipelineOp::put("key", "value"));
    ok = check_disconnected(client.pipeline(std::move(operations)),
                            "pipeline") && ok;
    auto close = client.close();
    if (!close.await_ready() || !close.await_resume()) {
        std::cerr << "closing a disconnected client must complete synchronously\n";
        ok = false;
    }
    return ok ? 0 : 1;
}
