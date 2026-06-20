#include <chrono>
#include <iostream>
#include <string>

#include <galay/cpp/galay-mongo/base/mongo_error.h>
#include <galay/cpp/galay-mongo/base/mongo_uri.h>

using namespace galay::mongo;

namespace
{

bool failCase(const std::string& message)
{
    std::cerr << "  FAILED: " << message << std::endl;
    return false;
}

bool expectError(const std::string& uri, MongoErrorType type)
{
    auto parsed = parseMongoUri(uri);
    if (parsed) {
        return failCase("expected URI parse failure for: " + uri);
    }
    if (parsed.error().type() != type) {
        return failCase("unexpected error type for " + uri + ": " +
                        parsed.error().message());
    }
    return true;
}

bool test_replica_set_uri_round_trips_to_config()
{
    std::cout << "Testing replica set URI parsing..." << std::endl;

    auto parsed = parseMongoUri(
        "mongodb://host1:27017,host2:27018/test"
        "?replicaSet=rs0&authSource=admin"
        "&readPreference=primaryPreferred&serverSelectionTimeoutMS=3000");
    if (!parsed) {
        return failCase("parse failed: " + parsed.error().message());
    }

    const MongoConfig& cfg = *parsed;
    if (cfg.host != "host1" || cfg.port != 27017) {
        return failCase("compatibility host/port not set to first seed");
    }
    if (cfg.seeds.size() != 2) {
        return failCase("seed count mismatch");
    }
    if (cfg.seeds[0].host != "host1" || cfg.seeds[0].port != 27017 ||
        cfg.seeds[1].host != "host2" || cfg.seeds[1].port != 27018) {
        return failCase("seed endpoints mismatch");
    }
    if (cfg.database != "test") {
        return failCase("database mismatch");
    }
    if (cfg.auth_database != "admin") {
        return failCase("authSource mismatch");
    }
    if (cfg.topology.replica_set_name != "rs0") {
        return failCase("replicaSet mismatch");
    }
    if (cfg.topology.read_preference != MongoReadPreference::kPrimaryPreferred) {
        return failCase("readPreference mismatch");
    }
    if (cfg.topology.server_selection_timeout != std::chrono::milliseconds(3000)) {
        return failCase("serverSelectionTimeoutMS mismatch");
    }

    std::cout << "  PASSED" << std::endl;
    return true;
}

bool test_invalid_uri_cases_return_typed_errors()
{
    std::cout << "Testing invalid URI handling..." << std::endl;

    if (!expectError("postgresql://host1/test", MONGO_ERROR_INVALID_PARAM)) {
        return false;
    }
    if (!expectError("mongodb:///test", MONGO_ERROR_INVALID_PARAM)) {
        return false;
    }
    if (!expectError("mongodb://host1:notaport/test", MONGO_ERROR_INVALID_PARAM)) {
        return false;
    }
    if (!expectError("mongodb://host1:70000/test", MONGO_ERROR_INVALID_PARAM)) {
        return false;
    }
    if (!expectError("mongodb://host1/test?readPreference=unknown",
                     MONGO_ERROR_INVALID_PARAM)) {
        return false;
    }
    if (!expectError("mongodb://host1/test?tls=true", MONGO_ERROR_UNSUPPORTED)) {
        return false;
    }

    std::cout << "  PASSED" << std::endl;
    return true;
}

} // namespace

int main()
{
    std::cout << "=== T12: Mongo URI Unit Tests ===" << std::endl;
    if (!test_replica_set_uri_round_trips_to_config()) {
        return 1;
    }
    if (!test_invalid_uri_cases_return_typed_errors()) {
        return 1;
    }
    std::cout << "\nAll Mongo URI tests PASSED!" << std::endl;
    return 0;
}
