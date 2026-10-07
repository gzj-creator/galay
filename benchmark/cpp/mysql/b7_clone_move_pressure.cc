#include "../common/benchmark_environment.h"

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

size_t parse_size_arg(int argc, char** argv, int index, size_t fallback)
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

std::string make_value(size_t value_size, char seed)
{
    std::string value;
    value.reserve(value_size);
    for (size_t i = 0; i < value_size; ++i) {
        value.push_back(static_cast<char>(seed + (i % 23)));
    }
    return value;
}

MysqlField make_field(size_t index, size_t value_size)
{
    MysqlField field("field_" + std::to_string(index) + "_" + make_value(value_size, 'a'),
                     MysqlFieldType::VAR_STRING,
                     NOT_NULL_FLAG,
                     static_cast<uint32_t>(value_size),
                     0);
    field.set_catalog(make_value(value_size, 'c'));
    field.set_schema(make_value(value_size, 's'));
    field.set_table(make_value(value_size, 't'));
    field.set_org_table(make_value(value_size, 'o'));
    field.set_org_name(make_value(value_size, 'n'));
    field.set_character_set(45);
    return field;
}

MysqlCommandBuilder make_builder(size_t command_count, size_t value_size)
{
    MysqlCommandBuilder builder;
    builder.reserve(command_count, command_count * (value_size + 16));
    for (size_t i = 0; i < command_count; ++i) {
        const std::string sql = "SELECT '" + make_value(value_size, 'a') + "'";
        builder.append_query(sql, static_cast<uint8_t>(i % 255));
    }
    return builder;
}

MysqlResultSet make_result_set(size_t field_count, size_t row_count, size_t value_size)
{
    MysqlResultSet result;
    result.reserve_fields(field_count);
    result.reserve_rows(row_count);
    for (size_t i = 0; i < field_count; ++i) {
        result.add_field(make_field(i, value_size));
    }
    for (size_t row_index = 0; row_index < row_count; ++row_index) {
        std::vector<std::optional<std::string>> values;
        values.reserve(field_count);
        for (size_t field_index = 0; field_index < field_count; ++field_index) {
            values.emplace_back(make_value(value_size, static_cast<char>('a' + field_index % 8)));
        }
        result.add_row(MysqlRow(std::move(values)));
    }
    result.set_affected_rows(row_count);
    result.set_last_insert_id(1000 + row_count);
    result.set_warnings(1);
    result.set_status_flags(2);
    result.set_info(make_value(value_size, 'i'));
    return result;
}

struct BenchResult
{
    uint64_t checksum = 0;
    long long elapsed_us = 0;
};

BenchResult run_builder_clone_move(const MysqlCommandBuilder& source, size_t iterations)
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

BenchResult run_encoded_batch_clone_move(const MysqlEncodedBatch& source, size_t iterations)
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

BenchResult run_result_set_clone_move(const MysqlResultSet& source, size_t iterations)
{
    BenchResult result;
    const auto started = std::chrono::steady_clock::now();
    for (size_t i = 0; i < iterations; ++i) {
        MysqlResultSet cloned = source.clone();
        MysqlResultSet moved = std::move(cloned);
        result.checksum += moved.field_count();
        result.checksum += moved.row_count();
        if (moved.field_count() > 0) {
            result.checksum += moved.field(0).name().size();
        }
        if (moved.row_count() > 0 && !moved.row(0).empty()) {
            result.checksum += moved.row(0).get_string(0).size();
        }
    }
    const auto finished = std::chrono::steady_clock::now();
    result.elapsed_us =
        std::chrono::duration_cast<std::chrono::microseconds>(finished - started).count();
    return result;
}

void print_rate(std::string_view label, const BenchResult& result, size_t iterations)
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
    if (!galay::benchmark::initialize_benchmark_environment()) {
        return 1;
    }

    const size_t iterations = parse_size_arg(argc, argv, 1, 10000);
    const size_t command_count = parse_size_arg(argc, argv, 2, 8);
    const size_t field_count = parse_size_arg(argc, argv, 3, 8);
    const size_t row_count = parse_size_arg(argc, argv, 4, 32);
    const size_t value_size = parse_size_arg(argc, argv, 5, 64);

    const MysqlCommandBuilder builder = make_builder(command_count, value_size);
    const MysqlEncodedBatch batch = builder.build();
    const MysqlResultSet result_set = make_result_set(field_count, row_count, value_size);

    const auto builder_result = run_builder_clone_move(builder, iterations);
    const auto batch_result = run_encoded_batch_clone_move(batch, iterations);
    const auto result_set_result = run_result_set_clone_move(result_set, iterations);

    std::cout << "MySQL clone/move pressure benchmark\n";
    std::cout << "Iterations: " << iterations << '\n';
    std::cout << "Commands: " << command_count << '\n';
    std::cout << "Fields: " << field_count << '\n';
    std::cout << "Rows: " << row_count << '\n';
    std::cout << "Value bytes: " << value_size << '\n';
    print_rate("Builder clone+move", builder_result, iterations);
    print_rate("Encoded batch clone+move", batch_result, iterations);
    print_rate("Result set clone+move", result_set_result, iterations);

    return builder_result.checksum != 0 &&
           batch_result.checksum != 0 &&
           result_set_result.checksum != 0 ? 0 : 1;
}
