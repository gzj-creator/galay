#include <serde/json/json.hpp>
#include <serde/json/schema.hpp>
#include <cassert>
#include <iostream>

namespace wire_test {
struct Message {
    std::variant<int64_t, std::string> id;
    std::optional<std::string> result;
    std::string schema;
    bool enabled = false;
    bool failed = false;
};
constexpr auto reflect_fields(std::type_identity<Message>) {
    return std::make_tuple(
        json::make_field("id", &Message::id),
        json::make_field("result", &Message::result, json::FieldPolicy{.raw = true, .omit_empty = true}),
        json::make_field("schema", &Message::schema, json::FieldPolicy{.raw = true, .object_only = true}),
        json::make_field("enabled", &Message::enabled, json::FieldPolicy{.optional = true, .presence_object = true}),
        json::make_field("failed", &Message::failed, json::FieldPolicy{.optional = true, .omit_false = true}));
}
struct Objects {
    std::vector<std::string> items;
};
constexpr auto reflect_fields(std::type_identity<Objects>) {
    return std::make_tuple(json::make_field("items", &Objects::items,
        json::FieldPolicy{.raw = true, .object_only = true}));
}
}

int main() {
    using wire_test::Message;
    const auto decoded = json::deserialize<Message>(R"({"id":"request","result":null,"schema":{},"enabled":{}})");
    if (!decoded) std::cerr << decoded.error() << '\n';
    assert(decoded && std::get<std::string>(decoded->id) == "request");
    assert(decoded->result && *decoded->result == "null" && decoded->enabled);
    auto encoded = json::serialize(*decoded);
    assert(encoded);
    auto round_trip = json::deserialize<Message>(*encoded);
    assert(round_trip && round_trip->result == decoded->result);
    assert(encoded->find("failed") == std::string::npos);
    assert(!json::deserialize<Message>(R"({"id":null,"schema":{}})"));
    assert(!json::deserialize<Message>(R"({"id":1,"schema":[]})"));
    assert(!json::serialize(Message{int64_t{1}, std::nullopt, "[]"}));
    const auto first = json::deserialize<json::Json>(R"({"value":"first"})");
    const auto second = json::deserialize<json::Json>(R"({"value":"second"})");
    assert(first && second && first->at("value").as_string() == "first");
    assert(json::serialize(*first) == R"({"value":"first"})");
    assert(json::deserialize<wire_test::Objects>(R"({"items":[{}, {"x":1}]})"));
    assert(!json::deserialize<wire_test::Objects>(R"({"items":[{}, 1]})"));
    assert(!json::serialize(wire_test::Objects{{"{}", "1"}}));
    auto invalid = json::serialize(json::Json{});
    assert(!invalid);
    json::SerializeOptions limited;
    limited.max_string_bytes = 2;
    assert(!json::serialize(*first, limited));
    limited = {};
    limited.max_array_items = 1;
    assert(!json::serialize(wire_test::Objects{{"{}", "{}"}}, limited));
    limited = {};
    limited.max_nodes = 1;
    const auto scalar = json::parse("1");
    assert(scalar && json::serialize(*scalar, limited) == "1");
    auto source = json::parse(R"({"nested":{"x":1}})");
    assert(source);
    auto owned = json::decode<json::Json>(source->at("nested"));
    source = json::parse("{}");
    assert(owned && json::serialize(*owned) == R"({"x":1})");

    json::SchemaBuilder schema;
    schema.add_string("name", "Name", true);
    auto clone = schema.clone();
    schema.add_integer("age", "Age");
    auto original_text = schema.encode();
    auto clone_text = clone.encode();
    assert(original_text && clone_text);
    assert(original_text->find("age") != std::string::npos);
    assert(clone_text->find("age") == std::string::npos);
    json::SchemaBuilder invalid_schema;
    invalid_schema.add_object("nested", "Nested", "[]");
    assert(!invalid_schema.encode());
    json::SchemaBuilder invalid_parent;
    invalid_parent.add_object("nested", "Nested", invalid_schema);
    assert(!invalid_parent.encode());
}
