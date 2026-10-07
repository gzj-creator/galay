#include <iostream>
#include <string>

#include <galay/cpp/galay-mongo/base/mongo_value.h>

using namespace galay::mongo;

namespace
{

bool fail_case(const std::string& message)
{
    std::cerr << "  FAILED: " << message << std::endl;
    return false;
}

bool test_document_clone_deep_copies_nested_document_and_array()
{
    std::cout << "Testing MongoDocument nested clone isolation..." << std::endl;

    MongoArray aliases;
    aliases.append("primary");
    aliases.append("secondary");

    MongoDocument profile;
    profile.append("city", "shanghai");
    profile.append("aliases", std::move(aliases));

    MongoDocument original;
    original.append("name", "galay");
    original.append("profile", std::move(profile));

    MongoDocument cloned = original.clone();

    MongoValue* profile_value = original.find("profile");
    if (profile_value == nullptr || !profile_value->is_document()) {
        return fail_case("original profile missing");
    }
    MongoDocument& original_profile = profile_value->as_document();
    original_profile.set("city", "beijing");

    MongoValue* aliases_value = original_profile.find("aliases");
    if (aliases_value == nullptr || !aliases_value->is_array()) {
        return fail_case("original aliases missing");
    }
    aliases_value->as_array().values()[0] = MongoValue("mutated");

    const auto* cloned_profile_value = cloned.find("profile");
    if (cloned_profile_value == nullptr || !cloned_profile_value->is_document()) {
        return fail_case("cloned profile missing");
    }
    const MongoDocument& cloned_profile = cloned_profile_value->to_document();
    if (cloned_profile.get_string("city") != "shanghai") {
        return fail_case("cloned nested document shared mutable state");
    }

    const auto* cloned_aliases_value = cloned_profile.find("aliases");
    if (cloned_aliases_value == nullptr || !cloned_aliases_value->is_array()) {
        return fail_case("cloned aliases missing");
    }
    const MongoArray& cloned_aliases = cloned_aliases_value->to_array();
    if (cloned_aliases.size() != 2 || cloned_aliases[0].to_string() != "primary") {
        return fail_case("cloned nested array shared mutable state");
    }

    std::cout << "  PASSED" << std::endl;
    return true;
}

bool test_value_clone_deep_copies_nested_array()
{
    std::cout << "Testing MongoValue array clone isolation..." << std::endl;

    MongoDocument item;
    item.append("state", "original");

    MongoArray original_array;
    original_array.append(std::move(item));

    MongoValue original(std::move(original_array));
    MongoValue cloned = original.clone();

    MongoDocument& original_item = original.as_array().values()[0].as_document();
    original_item.set("state", "mutated");

    const MongoArray& cloned_array = cloned.to_array();
    if (cloned_array.size() != 1 || !cloned_array[0].is_document()) {
        return fail_case("cloned value array shape mismatch");
    }
    if (cloned_array[0].to_document().get_string("state") != "original") {
        return fail_case("MongoValue clone shared nested array document state");
    }

    std::cout << "  PASSED" << std::endl;
    return true;
}

bool test_reply_clone_deep_copies_document()
{
    std::cout << "Testing MongoReply clone isolation..." << std::endl;

    MongoDocument response;
    response.append("ok", int32_t(1));
    response.append("errmsg", "original");

    MongoReply original(std::move(response));
    MongoReply cloned = original.clone();

    original.document().set("ok", int32_t(0));
    original.document().set("errmsg", "mutated");

    if (!cloned.ok()) {
        return fail_case("cloned reply ok state changed after original mutation");
    }
    if (cloned.error_message() != "original") {
        return fail_case("cloned reply document shared mutable state");
    }

    std::cout << "  PASSED" << std::endl;
    return true;
}

} // namespace

int main()
{
    std::cout << "=== T16: Mongo clone semantics tests ===" << std::endl;
    if (!test_document_clone_deep_copies_nested_document_and_array()) {
        return 1;
    }
    if (!test_value_clone_deep_copies_nested_array()) {
        return 1;
    }
    if (!test_reply_clone_deep_copies_document()) {
        return 1;
    }
    std::cout << "\nAll clone semantics tests PASSED!" << std::endl;
    return 0;
}
