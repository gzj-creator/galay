#ifndef GALAY_API_SCHEMA_MODEL_H
#define GALAY_API_SCHEMA_MODEL_H

#include "api_error.h"
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace galay::api {

enum class SchemaUse { input, output };
using SchemaNumber = std::variant<std::int64_t, std::uint64_t, double>;
using SchemaValue = std::variant<bool, std::int64_t, std::uint64_t, double, std::string>;

struct Schema {
    std::string type;
    std::string format;
    std::string description;
    bool nullable = false;
    std::optional<SchemaNumber> minimum;
    std::optional<SchemaNumber> maximum;
    std::optional<std::size_t> min_length;
    std::optional<std::size_t> max_length;
    std::optional<std::size_t> min_items;
    std::optional<std::size_t> max_items;
    std::optional<std::size_t> min_properties;
    std::optional<std::size_t> max_properties;
    std::vector<SchemaValue> enum_values;
    std::map<std::string, Schema> properties;
    std::vector<std::string> required;
    std::shared_ptr<const Schema> items;
    std::shared_ptr<const Schema> additional_properties;
    bool allow_additional_properties = false;
};

ApiResult<std::string> schema_json(const Schema& schema);

} // namespace galay::api

#endif
