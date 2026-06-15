#ifndef GALAY_MONGO_TEST_REPLY_HELPER_H
#define GALAY_MONGO_TEST_REPLY_HELPER_H

#include "galay-mongo/base/mongo_value.h"

#include <expected>
#include <string>

namespace mongo_test
{

inline std::expected<size_t, std::string> firstBatchSize(const galay::mongo::MongoReply& reply)
{
    const auto* cursor = reply.document().find("cursor");
    if (cursor == nullptr || !cursor->isDocument()) {
        return std::unexpected("reply missing cursor document");
    }

    const auto* first_batch = cursor->toDocument().find("firstBatch");
    if (first_batch == nullptr || !first_batch->isArray()) {
        return std::unexpected("reply missing cursor.firstBatch array");
    }

    return first_batch->toArray().size();
}

inline std::expected<galay::mongo::MongoDocument, std::string>
firstBatchFrontDocument(const galay::mongo::MongoReply& reply)
{
    const auto* cursor = reply.document().find("cursor");
    if (cursor == nullptr || !cursor->isDocument()) {
        return std::unexpected("reply missing cursor document");
    }

    const auto* first_batch = cursor->toDocument().find("firstBatch");
    if (first_batch == nullptr || !first_batch->isArray()) {
        return std::unexpected("reply missing cursor.firstBatch array");
    }

    const auto& arr = first_batch->toArray();
    if (arr.empty()) {
        return std::unexpected("cursor.firstBatch is empty");
    }

    const auto& first = arr[0];
    if (!first.isDocument()) {
        return std::unexpected("cursor.firstBatch[0] is not document");
    }

    return first.toDocument();
}

} // namespace mongo_test

#endif // GALAY_MONGO_TEST_REPLY_HELPER_H
