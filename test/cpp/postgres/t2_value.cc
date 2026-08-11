#include <galay/cpp/galay-postgres/base/postgres_value.h>

#include <concepts>
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
consteval bool isMoveOnlyCloneable()
{
    return !std::is_copy_constructible_v<T> &&
           !std::is_copy_assignable_v<T> &&
           std::is_nothrow_move_constructible_v<T> &&
           std::is_nothrow_move_assignable_v<T> &&
           HasClone<T>;
}

static_assert(isMoveOnlyCloneable<PostgresField>());
static_assert(isMoveOnlyCloneable<PostgresRow>());
static_assert(isMoveOnlyCloneable<PostgresResultSet>());

std::string longString(char value)
{
    return std::string(160, value);
}

PostgresField makeField(std::string name)
{
    return PostgresField(std::move(name),
                         42,
                         3,
                         static_cast<uint32_t>(PostgresOid::VARCHAR),
                         -1,
                         17,
                         0);
}

void testOidAndFieldMetadata()
{
    require(static_cast<uint32_t>(PostgresOid::BOOL) == 16, "BOOL OID mismatch");
    require(static_cast<uint32_t>(PostgresOid::INT8) == 20, "INT8 OID mismatch");
    require(static_cast<uint32_t>(PostgresOid::INT4) == 23, "INT4 OID mismatch");
    require(static_cast<uint32_t>(PostgresOid::JSONB) == 3802, "JSONB OID mismatch");

    PostgresField field = makeField(longString('f'));
    require(field.tableOid() == 42, "table OID mismatch");
    require(field.columnIndex() == 3, "column index mismatch");
    require(field.typeOid() == static_cast<uint32_t>(PostgresOid::VARCHAR), "type OID mismatch");
    require(field.typeSize() == -1, "variable-width type size must remain signed");
    require(field.typeModifier() == 17, "type modifier mismatch");
    require(field.format() == 0, "text format mismatch");

    PostgresField clone = field.clone();
    require(clone.name() == field.name(), "field clone lost its name");
    require(clone.name().data() != field.name().data(), "field clone must own its name");

    const PostgresField extension_type("custom", 0, 0, 91042, -1, -1, 1);
    require(extension_type.typeOid() == 91042, "unknown extension OIDs must be preserved");
    require(extension_type.typeModifier() == -1, "negative type modifier must be preserved");
    require(extension_type.format() == 1, "binary format mismatch");
}

void testRowAccessAndConversion()
{
    std::vector<std::optional<std::string>> values;
    values.emplace_back(longString('a'));
    values.emplace_back(std::nullopt);
    values.emplace_back("-9223372036854775808");
    values.emplace_back("18446744073709551615");
    values.emplace_back("3.25");
    values.emplace_back("12x");

    PostgresRow row(std::move(values));
    require(row.size() == 6, "row size mismatch");
    require(!row.isNull(0) && row.isNull(1), "row NULL semantics mismatch");
    require(row.isNull(99), "out-of-range isNull must be safe");
    require(row.getString(1, "fallback") == "fallback", "NULL string fallback mismatch");
    require(row.getInt64(2, 7) == std::numeric_limits<int64_t>::min(), "int64 conversion mismatch");
    require(row.getUint64(3, 7) == std::numeric_limits<uint64_t>::max(), "uint64 conversion mismatch");
    require(row.getDouble(4, 7.0) == 3.25, "double conversion mismatch");
    require(row.getInt64(5, 77) == 77, "partial numeric parse must use fallback");
    require(row.getInt64(99, 88) == 88, "out-of-range numeric fallback mismatch");

    PostgresRow clone = row.clone();
    require(clone.values()[0] == row.values()[0], "row clone lost its value");
    require(clone.values()[0]->data() != row.values()[0]->data(),
            "row clone must own independent value storage");
}

void testResultSetCloneAndMetadata()
{
    PostgresResultSet result;
    result.reserveFields(1);
    result.reserveRows(1);
    result.addField(makeField(longString('n')));
    result.addRow(PostgresRow({std::optional<std::string>(longString('v'))}));
    result.setCommandTag("UPDATE 7");
    result.setAffectedRows(7);

    require(result.hasResultSet(), "result with fields must report a result set");
    require(result.fieldCount() == 1 && result.rowCount() == 1, "result dimensions mismatch");
    require(result.findField(result.field(0).name()) == 0, "field lookup mismatch");
    require(result.findField("missing") == -1, "missing field lookup mismatch");
    require(result.commandTag() == "UPDATE 7", "command tag mismatch");
    require(result.affectedRows() == 7, "affected row count mismatch");

    PostgresResultSet clone = result.clone();
    require(clone.field(0).name() == result.field(0).name(), "result clone lost field metadata");
    require(clone.row(0).getString(0) == result.row(0).getString(0), "result clone lost row data");
    require(clone.commandTag() == result.commandTag(), "result clone lost command tag");
    require(clone.field(0).name().data() != result.field(0).name().data(),
            "result clone must deep-copy field names");
    require(clone.row(0).values()[0]->data() != result.row(0).values()[0]->data(),
            "result clone must deep-copy row values");
    require(clone.commandTag().data() != result.commandTag().data(),
            "result clone must deep-copy command tag");

    PostgresResultSet command_only;
    command_only.setCommandTag("CREATE TABLE");
    require(!command_only.hasResultSet(), "command-only result must not report fields");
}

} // namespace

int main()
{
    testOidAndFieldMetadata();
    testRowAccessAndConversion();
    testResultSetCloneAndMetadata();
    return EXIT_SUCCESS;
}
