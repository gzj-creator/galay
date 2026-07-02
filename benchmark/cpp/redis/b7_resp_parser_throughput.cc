#include <galay/cpp/galay-redis/protoc/redis_protocol.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

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

std::string makeSimpleFrame(size_t payload_size)
{
    const std::string payload(payload_size, 's');
    return std::string("+") + payload + "\r\n";
}

std::string makeBulkFrame(size_t payload_size)
{
    const std::string payload(payload_size, 'b');
    return std::string("$") + std::to_string(payload_size) + "\r\n" + payload + "\r\n";
}

bool verifyFrame(std::string_view frame, RespType expected_type, size_t expected_payload_size)
{
    RespParser parser;
    RedisReply reply;
    auto parsed = parser.parseFast(frame.data(), frame.size(), &reply);
    if (!parsed) {
        std::cerr << "verify parse failed with " << static_cast<int>(parsed.error()) << "\n";
        return false;
    }
    if (parsed.value() != frame.size()) {
        std::cerr << "verify consumed " << parsed.value() << " of " << frame.size() << "\n";
        return false;
    }
    if (reply.getType() != expected_type) {
        std::cerr << "verify returned unexpected RESP type\n";
        return false;
    }
    const std::string value = reply.asString();
    if (value.size() != expected_payload_size) {
        std::cerr << "verify payload size mismatch\n";
        return false;
    }
    return true;
}

BenchmarkResult runParserLoop(std::string_view frame, RespType expected_type, size_t iterations)
{
    RespParser parser;
    std::uint64_t checksum = 0;

    const auto started = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        RedisReply reply;
        auto parsed = parser.parseFast(frame.data(), frame.size(), &reply);
        if (!parsed) {
            std::cerr << "benchmark parse failed with " << static_cast<int>(parsed.error()) << "\n";
            return BenchmarkResult{};
        }
        if (reply.getType() != expected_type) {
            std::cerr << "benchmark returned unexpected RESP type\n";
            return BenchmarkResult{};
        }
        checksum += static_cast<std::uint64_t>(parsed.value());
    }
    const auto finished = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(finished - started).count();
    return BenchmarkResult{true, seconds, checksum};
}

bool runScenario(const char* name,
                 std::string_view frame,
                 RespType expected_type,
                 size_t payload_size,
                 size_t iterations)
{
    if (!verifyFrame(frame, expected_type, payload_size)) {
        return false;
    }

    const size_t warmup_iterations = std::min<size_t>(iterations, 10000);
    const BenchmarkResult warmup = runParserLoop(frame, expected_type, warmup_iterations);
    if (!warmup.ok) {
        return false;
    }

    const BenchmarkResult measured = runParserLoop(frame, expected_type, iterations);
    if (!measured.ok || measured.seconds <= 0.0) {
        return false;
    }

    const double replies_per_second = static_cast<double>(iterations) / measured.seconds;
    const double ns_per_reply = measured.seconds * 1'000'000'000.0 / static_cast<double>(iterations);

    std::cout << name << " replies/sec=" << replies_per_second
              << " ns/reply=" << ns_per_reply
              << " checksum=" << measured.checksum << "\n";
    return true;
}

} // namespace

int main(int argc, char* argv[])
{
    size_t iterations = 1000000;
    size_t bulk_payload_size = 64;

    if (argc > 1 && !parseSizeArg(argv[1], &iterations)) {
        std::cerr << "usage: " << argv[0] << " [iterations] [bulk-payload-size]\n";
        return 2;
    }
    if (argc > 2 && !parseSizeArg(argv[2], &bulk_payload_size)) {
        std::cerr << "usage: " << argv[0] << " [iterations] [bulk-payload-size]\n";
        return 2;
    }

    const std::string simple_frame = makeSimpleFrame(2);
    const std::string bulk_frame = makeBulkFrame(bulk_payload_size);

    std::cout << "Redis RESP parser throughput\n"
              << "iterations=" << iterations
              << " bulk_payload_size=" << bulk_payload_size << "\n";

    if (!runScenario("simple-string", simple_frame, RespType::SimpleString, 2, iterations)) {
        return 1;
    }
    if (!runScenario("bulk-string", bulk_frame, RespType::BulkString, bulk_payload_size, iterations)) {
        return 1;
    }

    return 0;
}
