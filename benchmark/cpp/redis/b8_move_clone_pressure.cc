#include <galay/cpp/galay-redis/base/redis_value.h>
#include <galay/cpp/galay-redis/protoc/builder.h>
#include <galay/cpp/galay-redis/protoc/redis_protocol.h>

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using namespace galay::redis;
using namespace galay::redis::protocol;

namespace
{

struct BenchmarkResult
{
    bool ok = false;
    double seconds = 0.0;
    std::uint64_t checksum = 0;
};

bool parseSizeArg(std::string_view text, size_t* value)
{
    if (value == nullptr || text.empty()) {
        return false;
    }

    size_t parsed = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    auto [ptr, ec] = std::from_chars(begin, end, parsed);
    if (ec != std::errc() || ptr != end || parsed == 0) {
        return false;
    }

    *value = parsed;
    return true;
}

RedisReply makeNestedReply(size_t width, size_t payload_size)
{
    std::vector<RedisReply> outer;
    outer.reserve(width);

    for (size_t i = 0; i < width; ++i) {
        std::vector<std::pair<RedisReply, RedisReply>> map_entries;
        map_entries.reserve(2);

        std::string key = "key-" + std::to_string(i);
        std::string payload(payload_size, static_cast<char>('a' + (i % 26)));
        map_entries.emplace_back(RedisReply(RespType::SimpleString, std::move(key)),
                                 RedisReply(RespType::BulkString, std::move(payload)));
        map_entries.emplace_back(RedisReply(RespType::SimpleString, std::string("index")),
                                 RedisReply(RespType::Integer, static_cast<int64_t>(i)));

        outer.emplace_back(RespType::Map, std::move(map_entries));
    }

    return RedisReply(RespType::Array, std::move(outer));
}

RedisCommandBuilder makeCommandBuilder(size_t width, size_t payload_size)
{
    RedisCommandBuilder builder;
    builder.reserve(width, width * 2, width * (payload_size + 32));

    for (size_t i = 0; i < width; ++i) {
        const std::string key = "bench-key-" + std::to_string(i);
        const std::string value(payload_size, static_cast<char>('A' + (i % 26)));
        const std::array<std::string_view, 2> args{key, value};
        builder.append("SET", args);
    }

    return builder;
}

BenchmarkResult runReplyCloneMove(const RedisReply& source, size_t iterations)
{
    std::uint64_t checksum = 0;
    const auto started = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        RedisReply cloned = source.clone();
        RedisReply moved = std::move(cloned);
        checksum += moved.asArray().size();
    }
    const auto finished = std::chrono::steady_clock::now();
    return BenchmarkResult{true, std::chrono::duration<double>(finished - started).count(), checksum};
}

BenchmarkResult runValueCloneMove(const RedisValue& source, size_t iterations)
{
    std::uint64_t checksum = 0;
    const auto started = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        RedisValue cloned = source.clone();
        RedisValue moved = std::move(cloned);
        std::vector<RedisValue> values = moved.toArray();
        checksum += values.size();
    }
    const auto finished = std::chrono::steady_clock::now();
    return BenchmarkResult{true, std::chrono::duration<double>(finished - started).count(), checksum};
}

BenchmarkResult runBuilderCloneMove(const RedisCommandBuilder& source, size_t iterations)
{
    std::uint64_t checksum = 0;
    const auto started = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        RedisCommandBuilder cloned = source.clone();
        const auto cloned_views = cloned.commands();
        RedisCommandBuilder moved = std::move(cloned);
        const auto moved_views = moved.commands();
        checksum += moved.encoded().size() + cloned_views.size() + moved_views.size();
    }
    const auto finished = std::chrono::steady_clock::now();
    return BenchmarkResult{true, std::chrono::duration<double>(finished - started).count(), checksum};
}

BenchmarkResult runEncodedCommandCloneMove(const RedisEncodedCommand& source, size_t iterations)
{
    std::uint64_t checksum = 0;
    const auto started = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        RedisEncodedCommand cloned = source.clone();
        RedisEncodedCommand moved = std::move(cloned);
        checksum += moved.encoded.size() + moved.expected_replies;
    }
    const auto finished = std::chrono::steady_clock::now();
    return BenchmarkResult{true, std::chrono::duration<double>(finished - started).count(), checksum};
}

bool reportScenario(const char* name, BenchmarkResult result, size_t iterations)
{
    if (!result.ok || result.seconds <= 0.0) {
        std::cerr << name << " benchmark failed\n";
        return false;
    }

    const double ops_per_second = static_cast<double>(iterations) / result.seconds;
    const double ns_per_op = result.seconds * 1'000'000'000.0 / static_cast<double>(iterations);
    std::cout << name << " ops/sec=" << ops_per_second
              << " ns/op=" << ns_per_op
              << " checksum=" << result.checksum << "\n";
    return true;
}

} // namespace

int main(int argc, char* argv[])
{
    size_t iterations = 100000;
    size_t width = 8;
    size_t payload_size = 64;

    if (argc > 1 && !parseSizeArg(argv[1], &iterations)) {
        std::cerr << "usage: " << argv[0] << " [iterations] [width] [payload-size]\n";
        return 2;
    }
    if (argc > 2 && !parseSizeArg(argv[2], &width)) {
        std::cerr << "usage: " << argv[0] << " [iterations] [width] [payload-size]\n";
        return 2;
    }
    if (argc > 3 && !parseSizeArg(argv[3], &payload_size)) {
        std::cerr << "usage: " << argv[0] << " [iterations] [width] [payload-size]\n";
        return 2;
    }

    RedisReply reply = makeNestedReply(width, payload_size);
    RedisValue value(reply.clone());
    RedisCommandBuilder builder = makeCommandBuilder(width, payload_size);
    const auto cached_views = builder.commands();
    if (cached_views.size() != width) {
        std::cerr << "builder setup produced unexpected command count\n";
        return 1;
    }
    RedisEncodedCommand encoded = builder.build();

    std::cout << "Redis move/clone pressure\n"
              << "iterations=" << iterations
              << " width=" << width
              << " payload_size=" << payload_size << "\n";

    if (!reportScenario("redis-reply", runReplyCloneMove(reply, iterations), iterations)) {
        return 1;
    }
    if (!reportScenario("redis-value", runValueCloneMove(value, iterations), iterations)) {
        return 1;
    }
    if (!reportScenario("command-builder", runBuilderCloneMove(builder, iterations), iterations)) {
        return 1;
    }
    if (!reportScenario("encoded-command", runEncodedCommandCloneMove(encoded, iterations), iterations)) {
        return 1;
    }

    return 0;
}
