#include <chrono>
#include <iostream>
#include <string>

#include "galay-mongo/sync/mongo_client.h"
#include "config.h"
#include "reply_helper.h"

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
    std::cout << "=== T4: Sync Mongo Functional Tests ===" << std::endl;

    const auto test_cfg = mongo_test::loadMongoTestConfig();
    mongo_test::printMongoTestConfig(test_cfg);

    const std::string collection = "galay_mongo_sync_functional";
    const int64_t doc_id = makeUniqueId();

    MongoClient session;
    const auto cfg = mongo_test::toMongoConfig(test_cfg);

    auto fail = [&](const std::string& message) -> int {
        std::cerr << "FAIL: " << message << std::endl;
        session.close();
        return 1;
    };

    auto connected = session.connect(cfg);
    if (!connected) {
        return fail("connect failed: " + connected.error().message());
    }

    auto ping = session.ping(test_cfg.database);
    if (!ping) {
        return fail("ping failed: " + ping.error().message());
    }

    MongoDocument ping_command;
    ping_command.append("ping", int32_t(1));
    auto ping_by_command = session.command(test_cfg.database, ping_command);
    if (!ping_by_command) {
        return fail("command(ping) failed: " + ping_by_command.error().message());
    }

    MongoDocument invalid_command;
    invalid_command.append("galayUnknownCommand", int32_t(1));
    auto invalid_result = session.command(test_cfg.database, invalid_command);
    if (invalid_result) {
        return fail("invalid command should fail but succeeded");
    }
    if (invalid_result.error().type() != MONGO_ERROR_SERVER) {
        return fail("invalid command failed, but error type is not MONGO_ERROR_SERVER");
    }

    MongoDocument doc;
    doc.append("_id", doc_id);
    doc.append("name", "sync-functional");
    doc.append("counter", int32_t(1));
    doc.append("stage", "created");

    auto inserted = session.insertOne(test_cfg.database, collection, doc);
    if (!inserted) {
        return fail("insertOne failed: " + inserted.error().message());
    }

    MongoDocument filter;
    filter.append("_id", doc_id);

    auto found1 = session.findOne(test_cfg.database, collection, filter);
    if (!found1) {
        return fail("findOne(after insert) failed: " + found1.error().message());
    }

    const auto first_batch_size_1 = mongo_test::firstBatchSize(*found1);
    if (!first_batch_size_1) {
        return fail("findOne(after insert) parse failed: " + first_batch_size_1.error());
    }
    if (*first_batch_size_1 != 1) {
        return fail("findOne(after insert) expected firstBatch size=1");
    }

    const auto first_doc_1 = mongo_test::firstBatchFrontDocument(*found1);
    if (!first_doc_1) {
        return fail("findOne(after insert) first document parse failed: " + first_doc_1.error());
    }
    if (first_doc_1->getInt32("counter", -1) != 1 ||
        first_doc_1->getString("stage") != "created") {
        return fail("findOne(after insert) content mismatch");
    }

    MongoDocument set_doc;
    set_doc.append("counter", int32_t(2));
    set_doc.append("stage", "updated");

    MongoDocument update_doc;
    update_doc.append("$set", std::move(set_doc));

    auto updated = session.updateOne(test_cfg.database,
                                     collection,
                                     filter,
                                     update_doc,
                                     false);
    if (!updated) {
        return fail("updateOne failed: " + updated.error().message());
    }

    auto found2 = session.findOne(test_cfg.database, collection, filter);
    if (!found2) {
        return fail("findOne(after update) failed: " + found2.error().message());
    }

    const auto first_doc_2 = mongo_test::firstBatchFrontDocument(*found2);
    if (!first_doc_2) {
        return fail("findOne(after update) first document parse failed: " + first_doc_2.error());
    }
    if (first_doc_2->getInt32("counter", -1) != 2 ||
        first_doc_2->getString("stage") != "updated") {
        return fail("findOne(after update) content mismatch");
    }

    auto deleted = session.deleteOne(test_cfg.database, collection, filter);
    if (!deleted) {
        return fail("deleteOne failed: " + deleted.error().message());
    }

    auto found3 = session.findOne(test_cfg.database, collection, filter);
    if (!found3) {
        return fail("findOne(after delete) failed: " + found3.error().message());
    }

    const auto first_batch_size_3 = mongo_test::firstBatchSize(*found3);
    if (!first_batch_size_3) {
        return fail("findOne(after delete) parse failed: " + first_batch_size_3.error());
    }
    if (*first_batch_size_3 != 0) {
        return fail("findOne(after delete) expected firstBatch size=0");
    }

    session.close();
    std::cout << "T4 sync functional test OK" << std::endl;
    return 0;
}
