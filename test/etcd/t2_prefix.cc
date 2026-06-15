#include "galay-etcd/sync/etcd_client.h"

#include <chrono>
#include <iostream>
#include <string>
#include <vector>

using galay::etcd::EtcdConfig;
using galay::etcd::EtcdClient;

namespace
{

std::string nowSuffix()
{
    const auto now = std::chrono::high_resolution_clock::now().time_since_epoch();
    return std::to_string(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
}

int fail(const std::string& message)
{
    std::cerr << "[FAIL] " << message << '\n';
    return 1;
}

} // namespace

int main(int argc, char** argv)
{
    const std::string endpoint = argc > 1 ? argv[1] : "http://127.0.0.1:2379";

    EtcdConfig config;
    config.endpoint = endpoint;

    auto session = galay::etcd::EtcdClientBuilder().config(config).build();
    auto conn = session.connect();
    if (!conn.has_value()) {
        return fail("connect failed: " + conn.error().message());
    }

    const std::string prefix = "/galay-etcd/prefix/" + nowSuffix() + "/";
    const std::vector<std::pair<std::string, std::string>> kvs = {
        {prefix + "a", "1"},
        {prefix + "b", "2"},
        {prefix + "c", "3"},
    };

    for (const auto& [key, value] : kvs) {
        auto put = session.put(key, value);
        if (!put.has_value()) {
            return fail("put failed: " + key + " error=" + put.error().message());
        }
    }

    auto range = session.get(prefix, true);
    if (!range.has_value()) {
        return fail("prefix get failed: " + range.error().message());
    }
    if (range.value().size() < kvs.size()) {
        return fail("prefix get size mismatch");
    }
    std::cout << "[OK] prefix get count=" << range.value().size() << '\n';

    auto deleted = session.del(prefix, true);
    if (!deleted.has_value()) {
        return fail("prefix delete failed: " + deleted.error().message());
    }
    if (deleted.value() < static_cast<int64_t>(kvs.size())) {
        return fail("prefix delete count mismatch");
    }
    std::cout << "[OK] prefix delete count=" << deleted.value() << '\n';

    auto range_after = session.get(prefix, true);
    if (!range_after.has_value()) {
        return fail("prefix get after delete failed: " + range_after.error().message());
    }
    if (!range_after.value().empty()) {
        return fail("prefix keys should be empty after delete");
    }

    auto close = session.close();
    if (!close.has_value()) {
        return fail("close failed: " + close.error().message());
    }

    std::cout << "PREFIX TEST PASSED\n";
    return 0;
}
