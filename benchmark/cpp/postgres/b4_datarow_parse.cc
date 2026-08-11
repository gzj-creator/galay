#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <galay/cpp/galay-postgres/protoc/postgres_protocol.h>

using namespace galay::postgres;
using namespace galay::postgres::protocol;

namespace
{

constexpr size_t kMinimumColumnCount = 4;
constexpr size_t kMaximumPayloadBytes = 256U * 1024U * 1024U;
constexpr size_t kDefaultWarmupIterations = 10000;
constexpr std::uint64_t kHashOffset = 1469598103934665603ULL;
constexpr std::uint64_t kHashPrime = 1099511628211ULL;

struct RowFixture
{
    std::string payload;
    size_t null_columns = 0;
    size_t empty_columns = 0;
};

struct BenchResult
{
    double seconds = 0.0;
    std::uint64_t checksum = 0;
    bool ok = false;
};

bool parseSizeArg(std::string_view text, size_t* value) noexcept
{
    if (value == nullptr || text.empty()) {
        return false;
    }

    size_t parsed = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    const auto [ptr, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc() || ptr != end || parsed == 0) {
        return false;
    }

    *value = parsed;
    return true;
}

std::string makeTextValue(size_t value_size)
{
    std::string value(value_size, '\0');
    for (size_t index = 0; index < value_size; ++index) {
        value[index] = static_cast<char>('a' + (index % 26));
    }
    return value;
}

std::optional<std::string_view> columnValue(size_t index, std::string_view text_value)
{
    switch (index % 8) {
    case 0:
        return std::string_view("184467440737095516");
    case 1:
        return text_value;
    case 2:
        return std::nullopt;
    case 3:
        return std::string_view{};
    case 4:
        return std::string_view("2026-08-09 14:37:52.123456+08");
    case 5:
        return std::string_view("550e8400-e29b-41d4-a716-446655440000");
    case 6:
        return std::string_view(R"({"active":true,"score":98.5,"tags":["pgsql","wire"]})");
    default:
        return std::string_view("developer@example.com");
    }
}

bool makeRowFixture(size_t column_count, size_t value_size, RowFixture* fixture)
{
    if (fixture == nullptr || value_size > kMaximumPayloadBytes) {
        return false;
    }

    const std::string text_value = makeTextValue(value_size);
    std::vector<std::optional<std::string_view>> values;
    values.reserve(column_count);

    size_t payload_size = 2;
    for (size_t index = 0; index < column_count; ++index) {
        const auto value = columnValue(index, text_value);
        const size_t value_bytes = value.has_value() ? value->size() : 0;
        if (value_bytes > static_cast<size_t>(std::numeric_limits<std::int32_t>::max()) ||
            payload_size > kMaximumPayloadBytes - 4 ||
            value_bytes > kMaximumPayloadBytes - payload_size - 4) {
            return false;
        }
        payload_size += 4 + value_bytes;
        values.push_back(value);
    }

    fixture->payload.clear();
    fixture->payload.reserve(payload_size);
    fixture->null_columns = 0;
    fixture->empty_columns = 0;
    writeInt16(fixture->payload, static_cast<std::uint16_t>(column_count));
    for (const auto& value : values) {
        if (!value.has_value()) {
            writeInt32(fixture->payload, std::numeric_limits<std::uint32_t>::max());
            ++fixture->null_columns;
            continue;
        }

        writeInt32(fixture->payload, static_cast<std::uint32_t>(value->size()));
        fixture->payload.append(value->data(), value->size());
        if (value->empty()) {
            ++fixture->empty_columns;
        }
    }
    return fixture->payload.size() == payload_size;
}

void hashValue(std::uint64_t* hash, std::uint64_t value) noexcept
{
    *hash ^= value;
    *hash *= kHashPrime;
}

template <typename Values>
std::uint64_t checksumValues(const Values& values) noexcept
{
    std::uint64_t hash = kHashOffset;
    size_t index = 0;
    for (const auto& value : values) {
        hashValue(&hash, static_cast<std::uint64_t>(index++));
        if (!value.has_value()) {
            hashValue(&hash, std::numeric_limits<std::uint64_t>::max());
            continue;
        }

        hashValue(&hash, static_cast<std::uint64_t>(value->size()));
        if (!value->empty()) {
            hashValue(&hash, static_cast<unsigned char>(value->front()));
            hashValue(&hash, static_cast<unsigned char>((*value)[value->size() / 2]));
            hashValue(&hash, static_cast<unsigned char>(value->back()));
        }
    }
    return hash;
}

void doNotOptimize(std::uint64_t const& value) noexcept
{
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "r,m"(value) : "memory");
#else
    static volatile std::uint64_t sink = 0;
    sink = value;
#endif
}

BenchResult runOwned(std::string_view payload, size_t column_count, size_t iterations)
{
    PostgresParser parser;
    std::uint64_t checksum = 0;
    const auto started = std::chrono::steady_clock::now();
    for (size_t iteration = 0; iteration < iterations; ++iteration) {
        auto row = parser.parseDataRow(payload.data(), payload.size());
        if (!row || row->size() != column_count) {
            std::cerr << "owned DataRow parse failed at iteration " << iteration << '\n';
            return BenchResult{};
        }
        checksum ^= checksumValues(row->values()) +
                    kHashPrime * static_cast<std::uint64_t>(iteration + 1);
    }
    doNotOptimize(checksum);
    const auto finished = std::chrono::steady_clock::now();
    return BenchResult{
        std::chrono::duration<double>(finished - started).count(),
        checksum,
        true,
    };
}

BenchResult runView(std::string_view payload, size_t column_count, size_t iterations)
{
    PostgresParser parser;
    std::uint64_t checksum = 0;
    const auto started = std::chrono::steady_clock::now();
    for (size_t iteration = 0; iteration < iterations; ++iteration) {
        auto row = parser.parseDataRowView(payload.data(), payload.size());
        if (!row || row->size() != column_count) {
            std::cerr << "view DataRow parse failed at iteration " << iteration << '\n';
            return BenchResult{};
        }
        checksum ^= checksumValues(*row) +
                    kHashPrime * static_cast<std::uint64_t>(iteration + 1);
    }
    doNotOptimize(checksum);
    const auto finished = std::chrono::steady_clock::now();
    return BenchResult{
        std::chrono::duration<double>(finished - started).count(),
        checksum,
        true,
    };
}

void printResult(const char* label,
                 const BenchResult& result,
                 size_t iterations,
                 size_t payload_bytes)
{
    const double rows_per_second = result.seconds > 0.0
        ? static_cast<double>(iterations) / result.seconds
        : 0.0;
    const double nanoseconds_per_row = iterations > 0
        ? result.seconds * 1'000'000'000.0 / static_cast<double>(iterations)
        : 0.0;
    const double input_mib_per_second = result.seconds > 0.0
        ? static_cast<double>(payload_bytes) * static_cast<double>(iterations) /
              (result.seconds * 1024.0 * 1024.0)
        : 0.0;

    std::cout << label
              << " rows/sec=" << rows_per_second
              << " ns/row=" << nanoseconds_per_row
              << " input MiB/sec=" << input_mib_per_second
              << " checksum=" << result.checksum << '\n';
}

void printUsage(const char* program)
{
    std::cerr << "usage: " << program << " [iterations] [columns>=4] [text-value-bytes]\n";
}

} // namespace

int main(int argc, char** argv)
{
    size_t iterations = 250000;
    size_t column_count = 12;
    size_t value_size = 64;

    if (argc > 4 ||
        (argc > 1 && !parseSizeArg(argv[1], &iterations)) ||
        (argc > 2 && !parseSizeArg(argv[2], &column_count)) ||
        (argc > 3 && !parseSizeArg(argv[3], &value_size))) {
        printUsage(argv[0]);
        return 2;
    }
    if (column_count < kMinimumColumnCount ||
        column_count > static_cast<size_t>(std::numeric_limits<std::int16_t>::max())) {
        printUsage(argv[0]);
        return 2;
    }

    RowFixture fixture;
    if (!makeRowFixture(column_count, value_size, &fixture)) {
        std::cerr << "failed to build DataRow payload; keep the encoded payload below 256 MiB\n";
        return 2;
    }
    if (fixture.null_columns == 0 || fixture.empty_columns == 0) {
        std::cerr << "DataRow fixture must contain both NULL and empty-string columns\n";
        return 2;
    }

    const size_t warmup_iterations = std::min(iterations, kDefaultWarmupIterations);
    const BenchResult owned_warmup =
        runOwned(fixture.payload, column_count, warmup_iterations);
    const BenchResult view_warmup =
        runView(fixture.payload, column_count, warmup_iterations);
    if (!owned_warmup.ok || !view_warmup.ok ||
        owned_warmup.checksum != view_warmup.checksum) {
        std::cerr << "DataRow warmup failed or produced inconsistent results\n";
        return 1;
    }

    const BenchResult owned = runOwned(fixture.payload, column_count, iterations);
    const BenchResult view = runView(fixture.payload, column_count, iterations);
    if (!owned.ok || !view.ok || owned.seconds <= 0.0 || view.seconds <= 0.0 ||
        owned.checksum != view.checksum) {
        std::cerr << "DataRow measurement failed or produced inconsistent results\n";
        return 1;
    }

    std::cout << std::fixed << std::setprecision(2);
    std::cout << "PostgreSQL DataRow parser benchmark (hot payload)\n"
              << "iterations=" << iterations
              << " warmup_iterations=" << warmup_iterations
              << " columns=" << column_count
              << " text_value_bytes=" << value_size
              << " payload_bytes=" << fixture.payload.size()
              << " null_columns=" << fixture.null_columns
              << " empty_columns=" << fixture.empty_columns << '\n';
    printResult("owned", owned, iterations, fixture.payload.size());
    printResult("view", view, iterations, fixture.payload.size());
    std::cout << "view/owned throughput=" << owned.seconds / view.seconds << "x\n";
    return 0;
}
