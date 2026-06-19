#include <iostream>

#include "it_config.h"

int main()
{
    std::cout << "=== T11: Replica Set Discovery Integration Test ===" << std::endl;

    const auto cfg = mongo_test::loadMongoReplicaSetItConfig();

    std::string skip_reason;
    if (mongo_test::shouldSkipReplicaSetIt(cfg, &skip_reason)) {
        std::cout << "[SKIP] " << skip_reason
                  << " to run Mongo replica set discovery integration test" << std::endl;
        return 0;
    }

    std::cout << "Replica set seeds:";
    for (const auto& seed : cfg.seeds) {
        std::cout << " " << seed.host << ":" << seed.port;
    }
    std::cout << " replicaSet=" << cfg.replica_set_name
              << " serverSelectionTimeoutMS=" << cfg.server_selection_timeout_ms
              << std::endl;

    std::cerr << "FAIL: replica set discovery API is not implemented yet; "
              << "expected one primary and at least one secondary" << std::endl;
    return 1;
}
