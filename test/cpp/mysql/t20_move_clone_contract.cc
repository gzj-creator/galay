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
consteval bool is_move_only_cloneable()
{
    return !std::is_copy_constructible_v<T> &&
           !std::is_copy_assignable_v<T> &&
           std::is_move_constructible_v<T> &&
           std::is_move_assignable_v<T> &&
           HasClone<T>;
}

using DefaultPrepareResult = MysqlPrepareAwaitable<>::PrepareResult;

static_assert(is_move_only_cloneable<MysqlEncodedBatch>());
static_assert(is_move_only_cloneable<MysqlField>());
static_assert(is_move_only_cloneable<MysqlRow>());
static_assert(is_move_only_cloneable<MysqlResultSet>());
static_assert(is_move_only_cloneable<DefaultPrepareResult>());
static_assert(is_move_only_cloneable<MysqlCommandBuilder>());
static_assert(std::is_nothrow_move_constructible_v<MysqlCommandBuilder>);
static_assert(std::is_nothrow_move_assignable_v<MysqlCommandBuilder>);

std::string long_value(char fill)
{
    return std::string(128, fill);
}

MysqlField make_field(std::string name)
{
    MysqlField field(std::move(name), MysqlFieldType::VAR_STRING, NOT_NULL_FLAG, 255, 0);
    field.set_catalog(long_value('c'));
    field.set_schema(long_value('s'));
    field.set_table(long_value('t'));
    field.set_org_table(long_value('o'));
    field.set_org_name(long_value('n'));
    field.set_character_set(45);
    return field;
}

void test_command_builder_clone_rebuilds_views()
{
    MysqlCommandBuilder builder;
    builder.reserve(2, 128);
    builder.append_query("SELECT 1");
    builder.append_ping(3);

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

void test_field_clone_deep_copies_strings()
{
    MysqlField field = make_field(long_value('f'));
    MysqlField cloned = field.clone();

    require(cloned.name() == field.name(), "field clone should preserve name");
    require(cloned.catalog() == field.catalog(), "field clone should preserve catalog");
    require(cloned.schema() == field.schema(), "field clone should preserve schema");
    require(cloned.table() == field.table(), "field clone should preserve table");
    require(cloned.org_table() == field.org_table(), "field clone should preserve org table");
    require(cloned.org_name() == field.org_name(), "field clone should preserve org name");
    require(cloned.character_set() == field.character_set(),
            "field clone should preserve character set");
    require(cloned.name().data() != field.name().data(),
            "field clone should own a separate name buffer");
    require(cloned.catalog().data() != field.catalog().data(),
            "field clone should own a separate catalog buffer");
}

void test_row_clone_deep_copies_values()
{
    std::vector<std::optional<std::string>> values;
    values.emplace_back(long_value('a'));
    values.emplace_back(std::nullopt);
    values.emplace_back(long_value('b'));
    MysqlRow row(std::move(values));

    MysqlRow cloned = row.clone();
    require(cloned.size() == row.size(), "row clone should preserve column count");
    require(cloned.get_string(0) == row.get_string(0), "row clone should preserve first value");
    require(cloned.is_null(1), "row clone should preserve null value");
    require(cloned.get_string(2) == row.get_string(2), "row clone should preserve last value");
    require(cloned.values()[0]->data() != row.values()[0]->data(),
            "row clone should own separate first value buffer");
    require(cloned.values()[2]->data() != row.values()[2]->data(),
            "row clone should own separate last value buffer");
}

void test_result_set_clone_deep_copies_fields_and_rows()
{
    MysqlResultSet result;
    result.add_field(make_field(long_value('r')));
    std::vector<std::optional<std::string>> values;
    values.emplace_back(long_value('v'));
    result.add_row(MysqlRow(std::move(values)));
    result.set_affected_rows(7);
    result.set_last_insert_id(9);
    result.set_warnings(2);
    result.set_status_flags(3);
    result.set_info(long_value('i'));

    MysqlResultSet cloned = result.clone();
    require(cloned.field_count() == 1, "result clone should preserve fields");
    require(cloned.row_count() == 1, "result clone should preserve rows");
    require(cloned.field(0).name() == result.field(0).name(),
            "result clone should preserve field data");
    require(cloned.row(0).get_string(0) == result.row(0).get_string(0),
            "result clone should preserve row data");
    require(cloned.affected_rows() == 7, "result clone should preserve affected rows");
    require(cloned.last_insert_id() == 9, "result clone should preserve insert id");
    require(cloned.warnings() == 2, "result clone should preserve warnings");
    require(cloned.status_flags() == 3, "result clone should preserve status flags");
    require(cloned.info() == result.info(), "result clone should preserve info");
    require(cloned.field(0).name().data() != result.field(0).name().data(),
            "result clone should deep-copy field buffers");
    require(cloned.row(0).values()[0]->data() != result.row(0).values()[0]->data(),
            "result clone should deep-copy row buffers");
    require(cloned.info().data() != result.info().data(),
            "result clone should deep-copy info buffer");
}

void test_prepare_result_clone_deep_copies_fields()
{
    DefaultPrepareResult result;
    result.statement_id = 42;
    result.num_params = 1;
    result.num_columns = 1;
    result.param_fields.push_back(make_field(long_value('p')));
    result.column_fields.push_back(make_field(long_value('q')));

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
    test_command_builder_clone_rebuilds_views();
    test_field_clone_deep_copies_strings();
    test_row_clone_deep_copies_values();
    test_result_set_clone_deep_copies_fields_and_rows();
    test_prepare_result_clone_deep_copies_fields();
    std::cout << "move/clone contract PASSED" << std::endl;
    return 0;
}
