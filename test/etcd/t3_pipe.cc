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

    const std::string base = "/galay-etcd/pipeline/" + nowSuffix();
    const std::string k1 = base + "/k1";
    const std::string k2 = base + "/k2";
    const std::string v1 = "seed";
    const std::string v2 = "pipeline-v2";

    auto seed = session.put(k1, v1);
    if (!seed.has_value()) {
        return fail("seed put failed: " + seed.error().message());
    }

    std::vector<EtcdClient::PipelineOp> ops;
    ops.push_back(EtcdClient::PipelineOp::Get(k1));
    ops.push_back(EtcdClient::PipelineOp::Put(k2, v2));
    ops.push_back(EtcdClient::PipelineOp::Del(k1));

    auto pipeline = session.pipeline(std::move(ops));
    if (!pipeline.has_value()) {
        return fail("pipeline failed: " + pipeline.error().message());
    }

    const auto& results = pipeline.value();
    if (results.size() != 3) {
        return fail("pipeline result size mismatch, expect=3, actual=" + std::to_string(results.size()));
    }

    if (results[0].kvs.empty() || results[0].kvs.front().value != v1) {
        return fail("pipeline get result mismatch");
    }
    if (!results[1].ok) {
        return fail("pipeline put result not ok");
    }
    if (results[2].deleted_count <= 0) {
        return fail("pipeline del result should delete at least one key");
    }
    std::cout << "[OK] pipeline responses parsed\n";

    auto verify_k1 = session.get(k1);
    if (!verify_k1.has_value()) {
        return fail("verify k1 failed: " + verify_k1.error().message());
    }
    if (!verify_k1.value().empty()) {
        return fail("k1 should be deleted by pipeline");
    }

    auto verify_k2 = session.get(k2);
    if (!verify_k2.has_value()) {
        return fail("verify k2 failed: " + verify_k2.error().message());
    }
    if (verify_k2.value().empty() || verify_k2.value().front().value != v2) {
        return fail("k2 should be written by pipeline");
    }

    (void)session.del(k2);

    auto close = session.close();
    if (!close.has_value()) {
        return fail("close failed: " + close.error().message());
    }

    std::cout << "PIPELINE TEST PASSED\n";
    return 0;
}
