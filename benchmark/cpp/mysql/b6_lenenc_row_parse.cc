#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <string>

#include <galay/cpp/galay-mysql/protoc/mysql_protocol.h>

using namespace galay::mysql::protocol;

namespace
{

size_t parseSizeArg(int argc, char** argv, int index, size_t fallback)
{
    if (argc <= index) {
        return fallback;
    }

    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(argv[index], &end, 10);
    if (end == argv[index] || *end != '\0' || parsed == 0 ||
        parsed > std::numeric_limits<size_t>::max()) {
        return fallback;
    }
    return static_cast<size_t>(parsed);
}

std::string makeValue(size_t value_size)
{
    std::string value;
    value.reserve(value_size);
    for (size_t i = 0; i < value_size; ++i) {
        value.push_back(static_cast<char>('a' + (i % 26)));
    }
    return value;
}

std::string makeRowPayload(size_t column_count, std::string_view value)
{
    std::string payload;
    payload.reserve(column_count * (value.size() + 1));
    for (size_t i = 0; i < column_count; ++i) {
        writeLenEncString(payload, value);
    }
    return payload;
}

struct BenchResult
{
    bool ok = false;
    uint64_t checksum = 0;
    long long elapsed_us = 0;
};

BenchResult runLenEncOwned(std::string_view payload, size_t iterations)
{
    BenchResult result;
    const auto started = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        size_t consumed = 0;
        auto parsed = readLenEncString(payload.data(), payload.size(), consumed);
        if (!parsed || consumed != payload.size()) {
            return result;
        }
        result.checksum += parsed->size();
        result.checksum += static_cast<unsigned char>(parsed->front());
    }
    const auto finished = std::chrono::steady_clock::now();
    result.elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(finished - started).count();
    result.ok = true;
    return result;
}

BenchResult runLenEncView(std::string_view payload, size_t iterations)
{
    BenchResult result;
    const auto started = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        size_t consumed = 0;
        auto parsed = readLenEncStringView(payload.data(), payload.size(), consumed);
        if (!parsed || consumed != payload.size()) {
            return result;
        }
        result.checksum += parsed->size();
        result.checksum += static_cast<unsigned char>(parsed->front());
    }
    const auto finished = std::chrono::steady_clock::now();
    result.elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(finished - started).count();
    result.ok = true;
    return result;
}

BenchResult runTextRowOwned(std::string_view payload, size_t column_count, size_t iterations)
{
    MysqlParser parser;
    BenchResult result;
    const auto started = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        auto row = parser.parseTextRow(payload.data(), payload.size(), column_count);
        if (!row || row->size() != column_count) {
            return result;
        }
        for (const auto& value : row.value()) {
            if (!value.has_value()) {
                return result;
            }
            result.checksum += value->size();
            result.checksum += static_cast<unsigned char>(value->front());
        }
    }
    const auto finished = std::chrono::steady_clock::now();
    result.elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(finished - started).count();
    result.ok = true;
    return result;
}

BenchResult runTextRowView(std::string_view payload, size_t column_count, size_t iterations)
{
    MysqlParser parser;
    BenchResult result;
    const auto started = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        auto row = parser.parseTextRowView(payload.data(), payload.size(), column_count);
        if (!row || row->size() != column_count) {
            return result;
        }
        for (const auto& value : row.value()) {
            if (!value.has_value()) {
                return result;
            }
            result.checksum += value->size();
            result.checksum += static_cast<unsigned char>(value->front());
        }
    }
    const auto finished = std::chrono::steady_clock::now();
    result.elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(finished - started).count();
    result.ok = true;
    return result;
}

void printRate(const char* label, const BenchResult& result, size_t operations)
{
    const double seconds = static_cast<double>(result.elapsed_us) / 1'000'000.0;
    const double ops_per_sec = seconds > 0.0 ? static_cast<double>(operations) / seconds : 0.0;
    std::cout << label << " elapsed us: " << result.elapsed_us << '\n';
    std::cout << label << " ops/sec: " << ops_per_sec << '\n';
    std::cout << label << " checksum: " << result.checksum << '\n';
}

} // namespace

int main(int argc, char** argv)
{
    const size_t iterations = parseSizeArg(argc, argv, 1, 100000);
    const size_t column_count = parseSizeArg(argc, argv, 2, 10);
    const size_t value_size = parseSizeArg(argc, argv, 3, 64);

    const std::string value = makeValue(value_size);
    std::string lenenc_payload;
    writeLenEncString(lenenc_payload, value);
    const std::string row_payload = makeRowPayload(column_count, value);

    const auto lenenc_owned = runLenEncOwned(lenenc_payload, iterations);
    const auto lenenc_view = runLenEncView(lenenc_payload, iterations);
    const auto row_owned = runTextRowOwned(row_payload, column_count, iterations);
    const auto row_view = runTextRowView(row_payload, column_count, iterations);

    std::cout << "MySQL length-encoded row parse benchmark\n";
    std::cout << "Iterations: " << iterations << '\n';
    std::cout << "Columns: " << column_count << '\n';
    std::cout << "Value bytes: " << value_size << '\n';
    printRate("Lenenc owned", lenenc_owned, iterations);
    printRate("Lenenc view", lenenc_view, iterations);
    printRate("Text row owned", row_owned, iterations);
    printRate("Text row view", row_view, iterations);

    return lenenc_owned.ok && lenenc_view.ok && row_owned.ok && row_view.ok ? 0 : 1;
}
