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

std::string makePayload(size_t size)
{
    std::string payload(size, '\0');
    for (size_t i = 0; i < size; ++i) {
        payload[i] = static_cast<char>('a' + (i % 26));
    }
    return payload;
}

} // namespace

int main()
{
    std::cout << "=== T7: Sync Large Message Bridge Tests ===" << std::endl;

    const auto test_cfg = mongo_test::loadMongoTestConfig();
    mongo_test::printMongoTestConfig(test_cfg);

    MongoConfig cfg = mongo_test::toMongoConfig(test_cfg);
    cfg.recv_buffer_size = 1024;
    std::cout << "Force recv_buffer_size for bridge test: " << cfg.recv_buffer_size << std::endl;

    const std::string collection = "galay_mongo_sync_large_bridge";
    const int64_t doc_id = makeUniqueId();
    const size_t payload_size = 96 * 1024;
    const std::string payload = makePayload(payload_size);

    MongoClient session;

    auto fail = [&](const std::string& message) -> int {
        std::cerr << "FAIL: " << message << std::endl;
        session.close();
        return 1;
    };

    auto connected = session.connect(cfg);
    if (!connected) {
        return fail("connect failed: " + connected.error().message());
    }

    MongoDocument doc;
    doc.append("_id", doc_id);
    doc.append("name", "large-message-bridge");
    doc.append("payload", payload);

    auto inserted = session.insertOne(test_cfg.database, collection, doc);
    if (!inserted) {
        return fail("insertOne failed: " + inserted.error().message());
    }

    MongoDocument filter;
    filter.append("_id", doc_id);

    auto found = session.findOne(test_cfg.database, collection, filter);
    if (!found) {
        return fail("findOne failed: " + found.error().message());
    }

    const auto first_batch_size = mongo_test::firstBatchSize(*found);
    if (!first_batch_size) {
        return fail("firstBatch parse failed: " + first_batch_size.error());
    }
    if (*first_batch_size != 1) {
        return fail("expected firstBatch size=1");
    }

    const auto first_doc = mongo_test::firstBatchFrontDocument(*found);
    if (!first_doc) {
        return fail("firstBatch first document parse failed: " + first_doc.error());
    }

    const std::string received_payload = first_doc->getString("payload");
    if (received_payload.size() != payload.size()) {
        return fail("payload size mismatch, expected=" +
                    std::to_string(payload.size()) +
                    " actual=" + std::to_string(received_payload.size()));
    }
    if (received_payload != payload) {
        return fail("payload content mismatch");
    }

    auto deleted = session.deleteOne(test_cfg.database, collection, filter);
    if (!deleted) {
        return fail("deleteOne failed: " + deleted.error().message());
    }

    session.close();
    std::cout << "T7 sync large message bridge test OK" << std::endl;
    return 0;
}
