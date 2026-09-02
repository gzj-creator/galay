#include <chrono>
#include <iostream>

#include "common/config.h"

import galay.mongo;

using namespace galay::mongo;

namespace
{

int64_t makeUniqueId()
{
    return static_cast<int64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

} // namespace

int main()
{
    const auto cfg = mongo_example::loadMongoConfigFromEnv();

    MongoClient session;
    auto connected = session.connect(cfg);
    if (!connected) {
        std::cerr << "Connect failed: " << connected.error().message() << std::endl;
        return 1;
    }

    const std::string collection = "galay_mongo_example_sync_crud";
    const int64_t doc_id = makeUniqueId();

    MongoDocument doc;
    doc.append("_id", doc_id);
    doc.append("name", "sync-example");
    doc.append("counter", int32_t(1));

    auto inserted = session.insertOne(cfg.database, collection, doc);
    if (!inserted) {
        std::cerr << "Insert failed: " << inserted.error().message() << std::endl;
        session.close();
        return 1;
    }

    MongoDocument filter;
    filter.append("_id", doc_id);

    auto found = session.findOne(cfg.database, collection, filter);
    if (!found) {
        std::cerr << "Find failed: " << found.error().message() << std::endl;
        session.close();
        return 1;
    }

    MongoDocument set_doc;
    set_doc.append("counter", int32_t(2));

    MongoDocument update_doc;
    update_doc.append("$set", std::move(set_doc));

    auto updated = session.updateOne(cfg.database, collection, filter, update_doc);
    if (!updated) {
        std::cerr << "Update failed: " << updated.error().message() << std::endl;
        session.close();
        return 1;
    }

    auto deleted = session.deleteOne(cfg.database, collection, filter);
    if (!deleted) {
        std::cerr << "Delete failed: " << deleted.error().message() << std::endl;
        session.close();
        return 1;
    }

    session.close();
    std::cout << "Sync CRUD example OK" << std::endl;
    return 0;
}
