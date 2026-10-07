#include "../common/benchmark_environment.h"

#include <galay/cpp/galay-mongo/base/mongo_value.h>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <expected>
#include <iostream>
#include <string>
#include <string_view>

namespace
{

std::expected<size_t, std::string> parse_positive_size(std::string_view text,
                                                     size_t fallback)
{
    if (text.empty()) {
        return fallback;
    }

    size_t value = 0;
    const auto* first = text.data();
    const auto* last = text.data() + text.size();
    const auto parsed = std::from_chars(first, last, value);
    if (parsed.ec != std::errc{} || parsed.ptr != last || value == 0) {
        return std::unexpected("expected a positive integer");
    }
    return value;
}

uint64_t mix_checksum(uint64_t seed, uint64_t value)
{
    seed ^= value + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
    return seed;
}

uint64_t checksum_value(const galay::mongo::MongoValue& value);

uint64_t checksum_array(const galay::mongo::MongoArray& array)
{
    uint64_t checksum = mix_checksum(1469598103934665603ULL, array.size());
    for (const auto& value : array.values()) {
        checksum = mix_checksum(checksum, checksum_value(value));
    }
    return checksum;
}

uint64_t checksum_document(const galay::mongo::MongoDocument& document)
{
    uint64_t checksum = mix_checksum(1099511628211ULL, document.size());
    for (const auto& [name, value] : document.fields()) {
        checksum = mix_checksum(checksum, name.size());
        checksum = mix_checksum(checksum, checksum_value(value));
    }
    return checksum;
}

uint64_t checksum_value(const galay::mongo::MongoValue& value)
{
    using galay::mongo::MongoValueType;

    uint64_t checksum = static_cast<uint64_t>(value.type());
    switch (value.type()) {
    case MongoValueType::Null:
        return checksum;
    case MongoValueType::Bool:
        return mix_checksum(checksum, value.to_bool(false) ? 1 : 0);
    case MongoValueType::Int32:
        return mix_checksum(checksum, static_cast<uint64_t>(value.to_int32()));
    case MongoValueType::Int64:
    case MongoValueType::DateTime:
    case MongoValueType::Timestamp:
        return mix_checksum(checksum, static_cast<uint64_t>(value.to_int64()));
    case MongoValueType::Double:
        return mix_checksum(checksum, static_cast<uint64_t>(value.to_double() * 1000.0));
    case MongoValueType::String:
    case MongoValueType::ObjectId:
        return mix_checksum(checksum, value.to_string().size());
    case MongoValueType::Binary:
        return mix_checksum(checksum, value.to_binary().size());
    case MongoValueType::Document:
        return mix_checksum(checksum, checksum_document(value.to_document()));
    case MongoValueType::Array:
        return mix_checksum(checksum, checksum_array(value.to_array()));
    }
    return checksum;
}

galay::mongo::MongoDocument make_seed_document(size_t field_count)
{
    galay::mongo::MongoDocument document;
    document.fields().reserve(field_count + 4);

    for (size_t i = 0; i < field_count; ++i) {
        document.append("field_" + std::to_string(i), static_cast<int64_t>(i * 17));
    }

    galay::mongo::MongoArray array;
    array.reserve(field_count);
    for (size_t i = 0; i < field_count; ++i) {
        galay::mongo::MongoDocument item;
        item.append("index", static_cast<int32_t>(i));
        item.append("label", "item_" + std::to_string(i));
        array.append(std::move(item));
    }

    galay::mongo::MongoDocument nested;
    nested.append("name", "clone-move-benchmark");
    nested.append("items", std::move(array));
    document.append("nested", std::move(nested));
    document.append("active", true);
    return document;
}

galay::mongo::MongoArray make_seed_array(size_t item_count)
{
    galay::mongo::MongoArray array;
    array.reserve(item_count);
    for (size_t i = 0; i < item_count; ++i) {
        galay::mongo::MongoDocument item;
        item.append("value", static_cast<int64_t>(i));
        item.append("text", "array_" + std::to_string(i));
        array.append(std::move(item));
    }
    return array;
}

} // namespace

int main(int argc, char** argv)
{
    if (!galay::benchmark::initialize_benchmark_environment()) {
        return 1;
    }

    size_t iterations = 50000;
    if (argc > 1) {
        auto parsed = parse_positive_size(argv[1], iterations);
        if (!parsed) {
            std::cerr << "invalid iterations: " << parsed.error() << '\n';
            return 1;
        }
        iterations = *parsed;
    }

    size_t payload_fields = 32;
    if (argc > 2) {
        auto parsed = parse_positive_size(argv[2], payload_fields);
        if (!parsed) {
            std::cerr << "invalid payload fields: " << parsed.error() << '\n';
            return 1;
        }
        payload_fields = *parsed;
    }

    const galay::mongo::MongoDocument seed_document = make_seed_document(payload_fields);
    const galay::mongo::MongoArray seed_array = make_seed_array(payload_fields);
    const galay::mongo::MongoValue seed_value(seed_document.clone());

    galay::mongo::MongoDocument reply_doc;
    reply_doc.append("ok", int32_t(1));
    reply_doc.append("payload", seed_document.clone());
    const galay::mongo::MongoReply seed_reply(std::move(reply_doc));

    uint64_t checksum = 0;
    constexpr size_t kWarmupIterations = 512;
    for (size_t i = 0; i < kWarmupIterations; ++i) {
        checksum = mix_checksum(checksum, checksum_value(seed_value.clone()));
        checksum = mix_checksum(checksum, checksum_array(seed_array.clone()));
        checksum = mix_checksum(checksum, checksum_document(seed_document.clone()));
        checksum = mix_checksum(checksum, checksum_document(seed_reply.clone().document()));
    }

    const auto start = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        galay::mongo::MongoValue value_clone = seed_value.clone();
        galay::mongo::MongoValue moved_value = std::move(value_clone);
        checksum = mix_checksum(checksum, checksum_value(moved_value));

        galay::mongo::MongoArray array_clone = seed_array.clone();
        galay::mongo::MongoArray moved_array = std::move(array_clone);
        checksum = mix_checksum(checksum, checksum_array(moved_array));

        galay::mongo::MongoDocument document_clone = seed_document.clone();
        galay::mongo::MongoDocument moved_document = std::move(document_clone);
        checksum = mix_checksum(checksum, checksum_document(moved_document));

        galay::mongo::MongoReply reply_clone = seed_reply.clone();
        galay::mongo::MongoReply moved_reply = std::move(reply_clone);
        checksum = mix_checksum(checksum, checksum_document(moved_reply.document()));
    }
    const auto finish = std::chrono::steady_clock::now();

    const auto elapsed_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(finish - start).count();
    const double ns_per_iteration =
        static_cast<double>(elapsed_ns) / static_cast<double>(iterations);

    std::cout << "B5-MongoCloneMoveOwnership iterations=" << iterations
              << " payload_fields=" << payload_fields << '\n';
    std::cout << "clone_move_ns_per_iteration=" << ns_per_iteration
              << " checksum=0x" << std::hex << checksum << std::dec << '\n';
    return 0;
}
