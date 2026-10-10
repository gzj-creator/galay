#include <galay/cpp/galay-postgres/base/postgres_value.h>

#include <concepts>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

using namespace galay::postgres;

namespace
{

void require(bool condition, std::string_view message)
{
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

template <typename T>
concept HasClone = requires(const T& value) {
    { value.clone() } -> std::same_as<T>;
};

template <typename T>
consteval bool is_move_only_cloneable()
{
    return !std::is_copy_constructible_v<T> &&
           !std::is_copy_assignable_v<T> &&
           std::is_nothrow_move_constructible_v<T> &&
           std::is_nothrow_move_assignable_v<T> &&
           HasClone<T>;
}

static_assert(is_move_only_cloneable<PostgresField>());
static_assert(is_move_only_cloneable<PostgresRow>());
static_assert(is_move_only_cloneable<PostgresResultSet>());

std::string long_string(char value)
{
    return std::string(160, value);
}

PostgresField make_field(std::string name)
{
    return PostgresField(std::move(name),
                         42,
                         3,
                         static_cast<uint32_t>(PostgresOid::VARCHAR),
                         -1,
                         17,
                         0);
}

void test_oid_and_field_metadata()
{
    require(static_cast<uint32_t>(PostgresOid::BOOL) == 16, "BOOL OID mismatch");
    require(static_cast<uint32_t>(PostgresOid::INT8) == 20, "INT8 OID mismatch");
    require(static_cast<uint32_t>(PostgresOid::INT4) == 23, "INT4 OID mismatch");
    require(static_cast<uint32_t>(PostgresOid::JSONB) == 3802, "JSONB OID mismatch");

    PostgresField field = make_field(long_string('f'));
    require(field.table_oid() == 42, "table OID mismatch");
    require(field.column_index() == 3, "column index mismatch");
    require(field.type_oid() == static_cast<uint32_t>(PostgresOid::VARCHAR), "type OID mismatch");
    require(field.type_size() == -1, "variable-width type size must remain signed");
    require(field.type_modifier() == 17, "type modifier mismatch");
    require(field.format() == 0, "text format mismatch");

    PostgresField clone = field.clone();
    require(clone.name() == field.name(), "field clone lost its name");
    require(clone.name().data() != field.name().data(), "field clone must own its name");

    const PostgresField extension_type("custom", 0, 0, 91042, -1, -1, 1);
    require(extension_type.type_oid() == 91042, "unknown extension OIDs must be preserved");
    require(extension_type.type_modifier() == -1, "negative type modifier must be preserved");
    require(extension_type.format() == 1, "binary format mismatch");
}

void test_row_access_and_conversion()
{
    std::vector<std::optional<std::string>> values;
    values.emplace_back(long_string('a'));
    values.emplace_back(std::nullopt);
    values.emplace_back("-9223372036854775808");
    values.emplace_back("18446744073709551615");
    values.emplace_back("3.25");
    values.emplace_back("12x");

    PostgresRow row(std::move(values));
    require(row.size() == 6, "row size mismatch");
    require(!row.is_null(0) && row.is_null(1), "row NULL semantics mismatch");
    require(row.is_null(99), "out-of-range isNull must be safe");
    require(row.get_string(1, "fallback") == "fallback", "NULL string fallback mismatch");
    require(row.get_int64(2, 7) == std::numeric_limits<int64_t>::min(), "int64 conversion mismatch");
    require(row.get_uint64(3, 7) == std::numeric_limits<uint64_t>::max(), "uint64 conversion mismatch");
    require(row.get_double(4, 7.0) == 3.25, "double conversion mismatch");
    require(row.get_int64(5, 77) == 77, "partial numeric parse must use fallback");
    require(row.get_int64(99, 88) == 88, "out-of-range numeric fallback mismatch");

    PostgresRow clone = row.clone();
    require(clone.values()[0] == row.values()[0], "row clone lost its value");
    require(clone.values()[0]->data() != row.values()[0]->data(),
            "row clone must own independent value storage");
}

void test_result_set_clone_and_metadata()
{
    PostgresResultSet result;
    result.reserve_fields(1);
    result.reserve_rows(1);
    result.add_field(make_field(long_string('n')));
    result.add_row(PostgresRow({std::optional<std::string>(long_string('v'))}));
    result.set_command_tag("UPDATE 7");
    result.set_affected_rows(7);

    require(result.has_result_set(), "result with fields must report a result set");
    require(result.field_count() == 1 && result.row_count() == 1, "result dimensions mismatch");
    require(result.find_field(result.field(0).name()) == 0, "field lookup mismatch");
    require(result.find_field("missing") == -1, "missing field lookup mismatch");
    require(result.command_tag() == "UPDATE 7", "command tag mismatch");
    require(result.affected_rows() == 7, "affected row count mismatch");

    PostgresResultSet clone = result.clone();
    require(clone.field(0).name() == result.field(0).name(), "result clone lost field metadata");
    require(clone.row(0).get_string(0) == result.row(0).get_string(0), "result clone lost row data");
    require(clone.command_tag() == result.command_tag(), "result clone lost command tag");
    require(clone.field(0).name().data() != result.field(0).name().data(),
            "result clone must deep-copy field names");
    require(clone.row(0).values()[0]->data() != result.row(0).values()[0]->data(),
            "result clone must deep-copy row values");
    require(clone.command_tag().data() != result.command_tag().data(),
            "result clone must deep-copy command tag");

    PostgresResultSet command_only;
    command_only.set_command_tag("CREATE TABLE");
    require(!command_only.has_result_set(), "command-only result must not report fields");
}

void test_double_conversion_boundaries()
{
    const auto converted = [](std::string value) {
        const PostgresRow row({std::move(value)});
        return row.get_double(0, 77.0);
    };
    require(converted("-1.25e2") == -125.0, "double exponent conversion");
    require(converted("4.9406564584124654e-324") == std::numeric_limits<double>::denorm_min(),
            "double subnormal conversion");
    require(std::signbit(converted("-0")), "double negative zero conversion");
    require(std::isinf(converted("Infinity")), "PostgreSQL infinity conversion");
    require(std::isnan(converted("NaN")), "PostgreSQL NaN conversion");
    for (const std::string value : {"", " 1.25", "+1.25", "1.25tail", "1,25", "1e999", "1e-999"}) {
        require(converted(value) == 77.0, "invalid double must use default");
    }
}

} // namespace

int main()
{
    test_oid_and_field_metadata();
    test_row_access_and_conversion();
    test_double_conversion_boundaries();
    test_result_set_clone_and_metadata();
    return EXIT_SUCCESS;
}
