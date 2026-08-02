#include <iostream>
#include <string>

#include "it_config.h"

#include <galay/cpp/galay-mongo/sync/mongo_client.h>

namespace
{

bool isPrimary(const galay::mongo::MongoDocument& hello)
{
    return hello.getBool("isWritablePrimary", false) ||
           hello.getBool("ismaster", false);
}

bool verifySelectedRole(const mongo_test::MongoReplicaSetItConfig& it_cfg,
                        galay::mongo::MongoReadPreference preference,
                        bool expect_primary)
{
    auto config = mongo_test::toMongoConfig(it_cfg.mongo);
    config.seeds.reserve(it_cfg.seeds.size());
    for (const auto& seed : it_cfg.seeds) {
        config.seeds.push_back({seed.host, seed.port});
    }
    config.host = config.seeds.front().host;
    config.port = config.seeds.front().port;
    config.topology.replica_set_name = it_cfg.replica_set_name;
    config.topology.server_selection_timeout =
        std::chrono::milliseconds(it_cfg.server_selection_timeout_ms);
    config.topology.read_preference = preference;

    galay::mongo::MongoClient client;
    auto connected = client.connect(config);
    if (!connected) {
        std::cerr << "FAIL: topology connect failed: "
                  << connected.error().message() << std::endl;
        return false;
    }

    galay::mongo::MongoDocument command;
    command.append("hello", int32_t(1));
    auto hello = client.command(config.hello_database, command);
    if (!hello) {
        std::cerr << "FAIL: selected server hello failed: "
                  << hello.error().message() << std::endl;
        return false;
    }

    const auto& document = hello->document();
    if (document.getString("setName") != it_cfg.replica_set_name) {
        std::cerr << "FAIL: selected server replica set mismatch" << std::endl;
        return false;
    }

    const bool primary = isPrimary(document);
    const bool secondary = document.getBool("secondary", false);
    if ((expect_primary && !primary) || (!expect_primary && !secondary)) {
        std::cerr << "FAIL: selected server role mismatch: primary=" << primary
                  << " secondary=" << secondary << std::endl;
        return false;
    }

    return true;
}

} // namespace

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

    size_t primary_count = 0;
    size_t secondary_count = 0;
    for (const auto& seed : cfg.seeds) {
        galay::mongo::MongoClient client;
        auto config = mongo_test::toMongoConfig(cfg.mongo);
        config.host = seed.host;
        config.port = seed.port;

        auto connected = client.connect(config);
        if (!connected) {
            std::cerr << "FAIL: seed connect failed for " << seed.host << ":"
                      << seed.port << ": " << connected.error().message() << std::endl;
            return 1;
        }

        galay::mongo::MongoDocument command;
        command.append("hello", int32_t(1));
        auto hello = client.command(config.hello_database, command);
        if (!hello) {
            std::cerr << "FAIL: seed hello failed for " << seed.host << ":"
                      << seed.port << ": " << hello.error().message() << std::endl;
            return 1;
        }

        const auto& document = hello->document();
        if (document.getString("setName") != cfg.replica_set_name) {
            std::cerr << "FAIL: seed replica set mismatch for " << seed.host << ":"
                      << seed.port << std::endl;
            return 1;
        }

        primary_count += isPrimary(document) ? 1U : 0U;
        secondary_count += document.getBool("secondary", false) ? 1U : 0U;
    }

    if (primary_count != 1 || secondary_count < 1) {
        std::cerr << "FAIL: expected one primary and at least one secondary, got primary="
                  << primary_count << " secondary=" << secondary_count << std::endl;
        return 1;
    }

    if (!verifySelectedRole(cfg, galay::mongo::MongoReadPreference::kPrimary, true)) {
        return 1;
    }
    if (!verifySelectedRole(cfg, galay::mongo::MongoReadPreference::kSecondary, false)) {
        return 1;
    }

    auto single_seed_cfg = cfg;
    single_seed_cfg.seeds.resize(1);
    if (!verifySelectedRole(single_seed_cfg,
                            galay::mongo::MongoReadPreference::kPrimary,
                            true)) {
        std::cerr << "FAIL: primary was not discovered from a single seed" << std::endl;
        return 1;
    }
    if (!verifySelectedRole(single_seed_cfg,
                            galay::mongo::MongoReadPreference::kSecondary,
                            false)) {
        std::cerr << "FAIL: secondary was not discovered from a single seed" << std::endl;
        return 1;
    }

    auto partial_outage_cfg = single_seed_cfg;
    partial_outage_cfg.seeds.insert(partial_outage_cfg.seeds.begin(), {"127.0.0.1", 1});
    if (!verifySelectedRole(partial_outage_cfg,
                            galay::mongo::MongoReadPreference::kPrimary,
                            true)) {
        std::cerr << "FAIL: healthy member was not discovered after a failed seed" << std::endl;
        return 1;
    }

    auto mismatch_config = mongo_test::toMongoConfig(cfg.mongo);
    for (const auto& seed : cfg.seeds) {
        mismatch_config.seeds.push_back({seed.host, seed.port});
    }
    mismatch_config.topology.replica_set_name = cfg.replica_set_name + "_wrong";
    mismatch_config.topology.server_selection_timeout = std::chrono::milliseconds(200);
    galay::mongo::MongoClient mismatch_client;
    auto mismatch_result = mismatch_client.connect(mismatch_config);
    if (mismatch_result || mismatch_result.error().type() != galay::mongo::MONGO_ERROR_TIMEOUT) {
        std::cerr << "FAIL: replica set mismatch did not return server selection timeout"
                  << std::endl;
        return 1;
    }

    std::cout << "Replica set discovery integration test PASSED: primary="
              << primary_count << " secondary=" << secondary_count << std::endl;
    return 0;
}
