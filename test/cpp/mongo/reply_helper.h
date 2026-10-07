#ifndef GALAY_MONGO_TEST_REPLY_HELPER_H
#define GALAY_MONGO_TEST_REPLY_HELPER_H

#include <galay/cpp/galay-mongo/base/mongo_value.h>

#include <expected>
#include <string>

namespace mongo_test
{

inline std::expected<size_t, std::string> first_batch_size(const galay::mongo::MongoReply& reply)
{
    const auto* cursor = reply.document().find("cursor");
    if (cursor == nullptr || !cursor->is_document()) {
        return std::unexpected("reply missing cursor document");
    }

    const auto* first_batch = cursor->to_document().find("firstBatch");
    if (first_batch == nullptr || !first_batch->is_array()) {
        return std::unexpected("reply missing cursor.firstBatch array");
    }

    return first_batch->to_array().size();
}

inline std::expected<galay::mongo::MongoDocument, std::string>
first_batch_front_document(const galay::mongo::MongoReply& reply)
{
    const auto* cursor = reply.document().find("cursor");
    if (cursor == nullptr || !cursor->is_document()) {
        return std::unexpected("reply missing cursor document");
    }

    const auto* first_batch = cursor->to_document().find("firstBatch");
    if (first_batch == nullptr || !first_batch->is_array()) {
        return std::unexpected("reply missing cursor.firstBatch array");
    }

    const auto& arr = first_batch->to_array();
    if (arr.empty()) {
        return std::unexpected("cursor.firstBatch is empty");
    }

    const auto& first = arr[0];
    if (!first.is_document()) {
        return std::unexpected("cursor.firstBatch[0] is not document");
    }

    return first.to_document().clone();
}

} // namespace mongo_test

#endif // GALAY_MONGO_TEST_REPLY_HELPER_H
