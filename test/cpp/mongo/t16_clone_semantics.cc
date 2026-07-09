#include <iostream>
#include <string>

#include <galay/cpp/galay-mongo/base/mongo_value.h>

using namespace galay::mongo;

namespace
{

bool failCase(const std::string& message)
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
    if (profile_value == nullptr || !profile_value->isDocument()) {
        return failCase("original profile missing");
    }
    MongoDocument& original_profile = profile_value->asDocument();
    original_profile.set("city", "beijing");

    MongoValue* aliases_value = original_profile.find("aliases");
    if (aliases_value == nullptr || !aliases_value->isArray()) {
        return failCase("original aliases missing");
    }
    aliases_value->asArray().values()[0] = MongoValue("mutated");

    const auto* cloned_profile_value = cloned.find("profile");
    if (cloned_profile_value == nullptr || !cloned_profile_value->isDocument()) {
        return failCase("cloned profile missing");
    }
    const MongoDocument& cloned_profile = cloned_profile_value->toDocument();
    if (cloned_profile.getString("city") != "shanghai") {
        return failCase("cloned nested document shared mutable state");
    }

    const auto* cloned_aliases_value = cloned_profile.find("aliases");
    if (cloned_aliases_value == nullptr || !cloned_aliases_value->isArray()) {
        return failCase("cloned aliases missing");
    }
    const MongoArray& cloned_aliases = cloned_aliases_value->toArray();
    if (cloned_aliases.size() != 2 || cloned_aliases[0].toString() != "primary") {
        return failCase("cloned nested array shared mutable state");
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

    MongoDocument& original_item = original.asArray().values()[0].asDocument();
    original_item.set("state", "mutated");

    const MongoArray& cloned_array = cloned.toArray();
    if (cloned_array.size() != 1 || !cloned_array[0].isDocument()) {
        return failCase("cloned value array shape mismatch");
    }
    if (cloned_array[0].toDocument().getString("state") != "original") {
        return failCase("MongoValue clone shared nested array document state");
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
        return failCase("cloned reply ok state changed after original mutation");
    }
    if (cloned.errorMessage() != "original") {
        return failCase("cloned reply document shared mutable state");
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
