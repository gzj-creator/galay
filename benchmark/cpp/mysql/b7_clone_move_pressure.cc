#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <galay/cpp/galay-mysql/base/mysql_value.h>
#include <galay/cpp/galay-mysql/protoc/builder.h>

using namespace galay::mysql;
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

std::string makeValue(size_t value_size, char seed)
{
    std::string value;
    value.reserve(value_size);
    for (size_t i = 0; i < value_size; ++i) {
        value.push_back(static_cast<char>(seed + (i % 23)));
    }
    return value;
}

MysqlField makeField(size_t index, size_t value_size)
{
    MysqlField field("field_" + std::to_string(index) + "_" + makeValue(value_size, 'a'),
                     MysqlFieldType::VAR_STRING,
                     NOT_NULL_FLAG,
                     static_cast<uint32_t>(value_size),
                     0);
    field.setCatalog(makeValue(value_size, 'c'));
    field.setSchema(makeValue(value_size, 's'));
    field.setTable(makeValue(value_size, 't'));
    field.setOrgTable(makeValue(value_size, 'o'));
    field.setOrgName(makeValue(value_size, 'n'));
    field.setCharacterSet(45);
    return field;
}

MysqlCommandBuilder makeBuilder(size_t command_count, size_t value_size)
{
    MysqlCommandBuilder builder;
    builder.reserve(command_count, command_count * (value_size + 16));
    for (size_t i = 0; i < command_count; ++i) {
        const std::string sql = "SELECT '" + makeValue(value_size, 'a') + "'";
        builder.appendQuery(sql, static_cast<uint8_t>(i % 255));
    }
    return builder;
}

MysqlResultSet makeResultSet(size_t field_count, size_t row_count, size_t value_size)
{
    MysqlResultSet result;
    result.reserveFields(field_count);
    result.reserveRows(row_count);
    for (size_t i = 0; i < field_count; ++i) {
        result.addField(makeField(i, value_size));
    }
    for (size_t row_index = 0; row_index < row_count; ++row_index) {
        std::vector<std::optional<std::string>> values;
        values.reserve(field_count);
        for (size_t field_index = 0; field_index < field_count; ++field_index) {
            values.emplace_back(makeValue(value_size, static_cast<char>('a' + field_index % 8)));
        }
        result.addRow(MysqlRow(std::move(values)));
    }
    result.setAffectedRows(row_count);
    result.setLastInsertId(1000 + row_count);
    result.setWarnings(1);
    result.setStatusFlags(2);
    result.setInfo(makeValue(value_size, 'i'));
    return result;
}

struct BenchResult
{
    uint64_t checksum = 0;
    long long elapsed_us = 0;
};

BenchResult runBuilderCloneMove(const MysqlCommandBuilder& source, size_t iterations)
{
    BenchResult result;
    const auto started = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        MysqlCommandBuilder cloned = source.clone();
        const auto cloned_views = cloned.commands();
        result.checksum += cloned_views.size();
        MysqlCommandBuilder moved = std::move(cloned);
        const auto moved_views = moved.commands();
        result.checksum += moved.size();
        result.checksum += moved.encoded().size();
        result.checksum += moved_views.empty() ? 0 : moved_views[0].encoded.size();
    }
    const auto finished = std::chrono::steady_clock::now();
    result.elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(finished - started).count();
    return result;
}

BenchResult runEncodedBatchCloneMove(const MysqlEncodedBatch& source, size_t iterations)
{
    BenchResult result;
    const auto started = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        MysqlEncodedBatch cloned = source.clone();
        MysqlEncodedBatch moved = std::move(cloned);
        result.checksum += moved.expected_responses;
        result.checksum += moved.encoded.size();
    }
    const auto finished = std::chrono::steady_clock::now();
    result.elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(finished - started).count();
    return result;
}

BenchResult runResultSetCloneMove(const MysqlResultSet& source, size_t iterations)
{
    BenchResult result;
    const auto started = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        MysqlResultSet cloned = source.clone();
        MysqlResultSet moved = std::move(cloned);
        result.checksum += moved.fieldCount();
        result.checksum += moved.rowCount();
        if (moved.fieldCount() > 0) {
            result.checksum += moved.field(0).name().size();
        }
        if (moved.rowCount() > 0 && !moved.row(0).empty()) {
            result.checksum += moved.row(0).getString(0).size();
        }
    }
    const auto finished = std::chrono::steady_clock::now();
    result.elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(finished - started).count();
    return result;
}

void printRate(std::string_view label, const BenchResult& result, size_t iterations)
{
    const double seconds = static_cast<double>(result.elapsed_us) / 1'000'000.0;
    const double ops_per_sec = seconds > 0.0 ? static_cast<double>(iterations) / seconds : 0.0;
    std::cout << label << " elapsed us: " << result.elapsed_us << '\n';
    std::cout << label << " ops/sec: " << ops_per_sec << '\n';
    std::cout << label << " checksum: " << result.checksum << '\n';
}

} // namespace

int main(int argc, char** argv)
{
    const size_t iterations = parseSizeArg(argc, argv, 1, 10000);
    const size_t command_count = parseSizeArg(argc, argv, 2, 8);
    const size_t field_count = parseSizeArg(argc, argv, 3, 8);
    const size_t row_count = parseSizeArg(argc, argv, 4, 32);
    const size_t value_size = parseSizeArg(argc, argv, 5, 64);

    const MysqlCommandBuilder builder = makeBuilder(command_count, value_size);
    const MysqlEncodedBatch batch = builder.build();
    const MysqlResultSet result_set = makeResultSet(field_count, row_count, value_size);

    const auto builder_result = runBuilderCloneMove(builder, iterations);
    const auto batch_result = runEncodedBatchCloneMove(batch, iterations);
    const auto result_set_result = runResultSetCloneMove(result_set, iterations);

    std::cout << "MySQL clone/move pressure benchmark\n";
    std::cout << "Iterations: " << iterations << '\n';
    std::cout << "Commands: " << command_count << '\n';
    std::cout << "Fields: " << field_count << '\n';
    std::cout << "Rows: " << row_count << '\n';
    std::cout << "Value bytes: " << value_size << '\n';
    printRate("Builder clone+move", builder_result, iterations);
    printRate("Encoded batch clone+move", batch_result, iterations);
    printRate("Result set clone+move", result_set_result, iterations);

    return builder_result.checksum != 0 &&
           batch_result.checksum != 0 &&
           result_set_result.checksum != 0 ? 0 : 1;
}
