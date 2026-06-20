#include <chrono>
#include <iostream>

#include <galay/cpp/galay-mongo/sync/mongo_client.h>
#include "config.h"

using namespace galay::mongo;

int main()
{
    std::cout << "=== T2: Sync Mongo Session Tests ===" << std::endl;

    const auto test_cfg = mongo_test::loadMongoTestConfig();
    mongo_test::printMongoTestConfig(test_cfg);

    MongoClient session;
    const auto cfg = mongo_test::toMongoConfig(test_cfg);

    std::cout << "Connecting to MongoDB..." << std::endl;
    auto connected = session.connect(cfg);
    if (!connected) {
        std::cerr << "Connect failed: " << connected.error().message() << std::endl;
        return 1;
    }
    std::cout << "Connected successfully." << std::endl;

    auto ping = session.ping(test_cfg.database);
    if (!ping) {
        std::cerr << "Ping failed: " << ping.error().message() << std::endl;
        session.close();
        return 1;
    }
    std::cout << "Ping OK." << std::endl;

    const int64_t id = static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    MongoDocument document;
    document.append("_id", id);
    document.append("name", "galay-mongo-sync-test");
    document.append("value", int32_t(42));

    auto insert = session.insertOne(test_cfg.database, "galay_mongo_test", document);
    if (!insert) {
        std::cerr << "Insert failed: " << insert.error().message() << std::endl;
        session.close();
        return 1;
    }
    std::cout << "Insert OK." << std::endl;

    MongoDocument filter;
    filter.append("_id", id);

    auto find = session.findOne(test_cfg.database, "galay_mongo_test", filter);
    if (!find) {
        std::cerr << "Find failed: " << find.error().message() << std::endl;
        session.close();
        return 1;
    }

    const auto* cursor = find->document().find("cursor");
    if (cursor && cursor->isDocument()) {
        const auto* first_batch = cursor->toDocument().find("firstBatch");
        if (first_batch && first_batch->isArray()) {
            std::cout << "Find firstBatch size: " << first_batch->toArray().size() << std::endl;
        }
    }

    auto del = session.deleteOne(test_cfg.database, "galay_mongo_test", filter);
    if (!del) {
        std::cerr << "Delete failed: " << del.error().message() << std::endl;
        session.close();
        return 1;
    }
    std::cout << "Delete OK." << std::endl;

    session.close();
    std::cout << "All sync Mongo tests completed." << std::endl;
    return 0;
}
