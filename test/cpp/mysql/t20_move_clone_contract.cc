#include <galay/cpp/galay-mysql/async/client.h>
#include <galay/cpp/galay-mysql/base/mysql_value.h>
#include <galay/cpp/galay-mysql/protoc/builder.h>

#include <cstdlib>
#include <concepts>
#include <iostream>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace galay::mysql;
using namespace galay::mysql::protocol;

namespace
{

void require(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << message << std::endl;
        std::exit(1);
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
           std::is_move_constructible_v<T> &&
           std::is_move_assignable_v<T> &&
           HasClone<T>;
}

using DefaultPrepareResult = MysqlPrepareAwaitable<>::PrepareResult;

static_assert(isMoveOnlyCloneable<MysqlEncodedBatch>());
static_assert(isMoveOnlyCloneable<MysqlField>());
static_assert(isMoveOnlyCloneable<MysqlRow>());
static_assert(isMoveOnlyCloneable<MysqlResultSet>());
static_assert(isMoveOnlyCloneable<DefaultPrepareResult>());
static_assert(isMoveOnlyCloneable<MysqlCommandBuilder>());
static_assert(std::is_nothrow_move_constructible_v<MysqlCommandBuilder>);
static_assert(std::is_nothrow_move_assignable_v<MysqlCommandBuilder>);

std::string longValue(char fill)
{
    return std::string(128, fill);
}

MysqlField makeField(std::string name)
{
    MysqlField field(std::move(name), MysqlFieldType::VAR_STRING, NOT_NULL_FLAG, 255, 0);
    field.setCatalog(longValue('c'));
    field.setSchema(longValue('s'));
    field.setTable(longValue('t'));
    field.setOrgTable(longValue('o'));
    field.setOrgName(longValue('n'));
    field.setCharacterSet(45);
    return field;
}

void testCommandBuilderCloneRebuildsViews()
{
    MysqlCommandBuilder builder;
    builder.reserve(2, 128);
    builder.appendQuery("SELECT 1");
    builder.appendPing(3);

    const auto original_views = builder.commands();
    require(original_views.size() == 2, "original builder should expose two commands");
    const char* original_first_view = original_views[0].encoded.data();

    MysqlCommandBuilder cloned = builder.clone();
    require(cloned.size() == builder.size(), "cloned builder should preserve command count");
    require(cloned.encoded() == builder.encoded(), "cloned builder should preserve encoded bytes");

    const auto cloned_views = cloned.commands();
    require(cloned_views.size() == 2, "cloned builder should expose two commands");
    require(cloned_views[0].encoded.data() == cloned.encoded().data(),
            "cloned builder view should point into cloned buffer");
    require(cloned_views[0].encoded.data() != original_first_view,
            "cloned builder view must not share original cached view");
    require(cloned_views[1].sequence_id == 3, "cloned builder should preserve sequence id");

    builder.clear();
    const auto cloned_views_after_clear = cloned.commands();
    require(cloned_views_after_clear.size() == 2,
            "cloned builder views should survive original clear");
    require(!cloned_views_after_clear[0].encoded.empty(),
            "cloned builder first view should remain valid");
    require(cloned_views_after_clear[0].encoded.data() == cloned.encoded().data(),
            "cloned builder cached view should remain bound to clone");
}

void testFieldCloneDeepCopiesStrings()
{
    MysqlField field = makeField(longValue('f'));
    MysqlField cloned = field.clone();

    require(cloned.name() == field.name(), "field clone should preserve name");
    require(cloned.catalog() == field.catalog(), "field clone should preserve catalog");
    require(cloned.schema() == field.schema(), "field clone should preserve schema");
    require(cloned.table() == field.table(), "field clone should preserve table");
    require(cloned.orgTable() == field.orgTable(), "field clone should preserve org table");
    require(cloned.orgName() == field.orgName(), "field clone should preserve org name");
    require(cloned.characterSet() == field.characterSet(),
            "field clone should preserve character set");
    require(cloned.name().data() != field.name().data(),
            "field clone should own a separate name buffer");
    require(cloned.catalog().data() != field.catalog().data(),
            "field clone should own a separate catalog buffer");
}

void testRowCloneDeepCopiesValues()
{
    std::vector<std::optional<std::string>> values;
    values.emplace_back(longValue('a'));
    values.emplace_back(std::nullopt);
    values.emplace_back(longValue('b'));
    MysqlRow row(std::move(values));

    MysqlRow cloned = row.clone();
    require(cloned.size() == row.size(), "row clone should preserve column count");
    require(cloned.getString(0) == row.getString(0), "row clone should preserve first value");
    require(cloned.isNull(1), "row clone should preserve null value");
    require(cloned.getString(2) == row.getString(2), "row clone should preserve last value");
    require(cloned.values()[0]->data() != row.values()[0]->data(),
            "row clone should own separate first value buffer");
    require(cloned.values()[2]->data() != row.values()[2]->data(),
            "row clone should own separate last value buffer");
}

void testResultSetCloneDeepCopiesFieldsAndRows()
{
    MysqlResultSet result;
    result.addField(makeField(longValue('r')));
    std::vector<std::optional<std::string>> values;
    values.emplace_back(longValue('v'));
    result.addRow(MysqlRow(std::move(values)));
    result.setAffectedRows(7);
    result.setLastInsertId(9);
    result.setWarnings(2);
    result.setStatusFlags(3);
    result.setInfo(longValue('i'));

    MysqlResultSet cloned = result.clone();
    require(cloned.fieldCount() == 1, "result clone should preserve fields");
    require(cloned.rowCount() == 1, "result clone should preserve rows");
    require(cloned.field(0).name() == result.field(0).name(),
            "result clone should preserve field data");
    require(cloned.row(0).getString(0) == result.row(0).getString(0),
            "result clone should preserve row data");
    require(cloned.affectedRows() == 7, "result clone should preserve affected rows");
    require(cloned.lastInsertId() == 9, "result clone should preserve insert id");
    require(cloned.warnings() == 2, "result clone should preserve warnings");
    require(cloned.statusFlags() == 3, "result clone should preserve status flags");
    require(cloned.info() == result.info(), "result clone should preserve info");
    require(cloned.field(0).name().data() != result.field(0).name().data(),
            "result clone should deep-copy field buffers");
    require(cloned.row(0).values()[0]->data() != result.row(0).values()[0]->data(),
            "result clone should deep-copy row buffers");
    require(cloned.info().data() != result.info().data(),
            "result clone should deep-copy info buffer");
}

void testPrepareResultCloneDeepCopiesFields()
{
    DefaultPrepareResult result;
    result.statement_id = 42;
    result.num_params = 1;
    result.num_columns = 1;
    result.param_fields.push_back(makeField(longValue('p')));
    result.column_fields.push_back(makeField(longValue('q')));

    DefaultPrepareResult cloned = result.clone();
    require(cloned.statement_id == 42, "prepare clone should preserve statement id");
    require(cloned.num_params == 1, "prepare clone should preserve param count");
    require(cloned.num_columns == 1, "prepare clone should preserve column count");
    require(cloned.param_fields.size() == 1, "prepare clone should preserve param fields");
    require(cloned.column_fields.size() == 1, "prepare clone should preserve column fields");
    require(cloned.param_fields[0].name() == result.param_fields[0].name(),
            "prepare clone should preserve param field data");
    require(cloned.param_fields[0].name().data() != result.param_fields[0].name().data(),
            "prepare clone should deep-copy param field buffers");
}

} // namespace

int main()
{
    testCommandBuilderCloneRebuildsViews();
    testFieldCloneDeepCopiesStrings();
    testRowCloneDeepCopiesValues();
    testResultSetCloneDeepCopiesFieldsAndRows();
    testPrepareResultCloneDeepCopiesFields();
    std::cout << "move/clone contract PASSED" << std::endl;
    return 0;
}
