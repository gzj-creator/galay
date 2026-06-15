#include "galay-etcd/sync/etcd_client.h"

#include <chrono>
#include <iostream>
#include <string>

int main(int argc, char** argv)
{
    const std::string endpoint = argc > 1 ? argv[1] : "http://127.0.0.1:2379";
    const std::string key = "/galay-etcd/examples/sync/" + std::to_string(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count());
    const std::string value = "hello-sync";

    galay::etcd::EtcdConfig cfg;
    cfg.endpoint = endpoint;

    auto client = galay::etcd::EtcdClientBuilder().config(cfg).build();
    auto conn = client.connect();
    if (!conn.has_value()) {
        std::cerr << "connect failed: " << conn.error().message() << '\n';
        return 1;
    }

    auto put = client.put(key, value);
    if (!put.has_value()) {
        std::cerr << "put failed: " << put.error().message() << '\n';
        return 2;
    }

    auto get = client.get(key);
    if (!get.has_value()) {
        std::cerr << "get failed: " << get.error().message() << '\n';
        return 3;
    }

    if (get.value().empty()) {
        std::cerr << "get returned empty kvs\n";
        return 4;
    }

    std::cout << "sync example ok: " << get.value().front().key
              << " => " << get.value().front().value << '\n';

    (void)client.del(key);
    (void)client.close();
    return 0;
}
