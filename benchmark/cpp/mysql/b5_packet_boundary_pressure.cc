#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

#include <galay/cpp/galay-mysql/protoc/builder.h>
#include <galay/cpp/galay-mysql/protoc/mysql_protocol.h>

using namespace galay::mysql::protocol;

namespace
{

size_t parseIterations(int argc, char** argv)
{
    if (argc < 2) {
        return 100000;
    }

    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(argv[1], &end, 10);
    if (end == argv[1] || *end != '\0' || parsed == 0) {
        return 100000;
    }
    return static_cast<size_t>(parsed);
}

} // namespace

int main(int argc, char** argv)
{
    const size_t iterations = parseIterations(argc, argv);
    const std::string valid_sql = "SELECT 1";
    const std::string oversized_sql(MYSQL_MAX_PACKET_SIZE, 'x');

    MysqlEncoder encoder;
    size_t valid_packets = 0;
    size_t rejected_packets = 0;
    size_t builder_invalid_slots = 0;

    const auto started = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        auto packet = encoder.encodeQuery(valid_sql);
        if (!packet.empty()) {
            ++valid_packets;
        }

        auto rejected = encoder.encodeQuery(oversized_sql);
        if (rejected.empty()) {
            ++rejected_packets;
        }

        MysqlCommandBuilder builder;
        builder.appendQuery(oversized_sql);
        const auto views = builder.commands();
        if (builder.size() == 1 && builder.encoded().empty() &&
            views.size() == 1 && views[0].encoded.empty()) {
            ++builder_invalid_slots;
        }
    }
    const auto finished = std::chrono::steady_clock::now();

    const auto elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(finished - started).count();
    const double seconds = static_cast<double>(elapsed_us) / 1'000'000.0;
    const double ops_per_sec = seconds > 0.0
        ? static_cast<double>(iterations * 3U) / seconds
        : 0.0;

    std::cout << "MySQL packet boundary pressure benchmark\n";
    std::cout << "Iterations: " << iterations << '\n';
    std::cout << "Valid packets: " << valid_packets << '\n';
    std::cout << "Encoder rejections: " << rejected_packets << '\n';
    std::cout << "Builder invalid slots: " << builder_invalid_slots << '\n';
    std::cout << "Elapsed us: " << elapsed_us << '\n';
    std::cout << "Ops/sec: " << ops_per_sec << '\n';

    return valid_packets == iterations &&
           rejected_packets == iterations &&
           builder_invalid_slots == iterations ? 0 : 1;
}
