#include <galay/cpp/galay-utils/system/env.hpp>

#include <iostream>

namespace {

using galay::utils::Env;

bool check(bool condition, const char* message)
{
    if (!condition) {
        std::cerr << "[environment] " << message << '\n';
    }
    return condition;
}

bool checkValue(const std::string& name, const std::string& expected)
{
    const auto value = Env::get(name);
    return check(value && value->has_value() && **value == expected,
                 "environment value should match");
}

bool checkMissing(const std::string& name)
{
    const auto value = Env::get(name);
    return check(value && !value->has_value(), "missing variable should return nullopt");
}

} // namespace

int main()
{
    const std::string name = "GALAY_UTILS_ENV_TEST_VAR";
    const auto original = Env::get(name);
    if (!check(original.has_value(), "read original environment")) {
        return 1;
    }

    bool ok = check(Env::unset(name).has_value(), "unset variable");
    ok = checkMissing(name) && ok;
    ok = check(Env::unset(name).has_value(), "unsetting missing variable succeeds") && ok;

    ok = check(Env::set(name, "initial", false).has_value(),
               "overwrite=false creates missing variable") && ok;
    ok = checkValue(name, "initial") && ok;
    const auto snapshot = Env::get(name);

    ok = check(Env::set(name, "ignored", false).has_value(),
               "overwrite=false succeeds for existing variable") && ok;
    ok = checkValue(name, "initial") && ok;
    ok = check(Env::set(name, "updated").has_value(), "overwrite defaults to true") && ok;
    ok = checkValue(name, "updated") && ok;
    ok = check(snapshot && snapshot->has_value() && **snapshot == "initial",
               "get returns an owned snapshot") && ok;

    ok = check(Env::set(name, "").has_value(), "setting empty value succeeds") && ok;
#if defined(_WIN32)
    // Windows CRT treats an empty value as deletion.
    ok = checkMissing(name) && ok;
#else
    ok = checkValue(name, "") && ok;
    ok = check(Env::set(name, "ignored", false).has_value(),
               "overwrite=false keeps an existing empty value") && ok;
    ok = checkValue(name, "") && ok;
#endif

    const std::string complexValue = " value=with spaces \n中文";
    ok = check(Env::set(name, complexValue, true).has_value(), "values may contain equals and whitespace") && ok;
    ok = checkValue(name, complexValue) && ok;
    const char* nativeValue = std::getenv(name.c_str());
    ok = check(nativeValue != nullptr && complexValue == nativeValue,
               "set updates the process environment") && ok;

    const std::string invalidNames[] = {
        "", "=INVALID", name + "=INVALID", name + std::string(1, '\0') + "INVALID",
    };
    for (const auto& invalidName : invalidNames) {
        const auto read = Env::get(invalidName);
        ok = check(!read && read.error() == std::errc::invalid_argument,
                   "get rejects invalid names") && ok;
        const auto write = Env::set(invalidName, "invalid");
        ok = check(!write && write.error() == std::errc::invalid_argument,
                   "set rejects invalid names") && ok;
        const auto writeWithoutOverwrite = Env::set(invalidName, "invalid", false);
        ok = check(!writeWithoutOverwrite && writeWithoutOverwrite.error() == std::errc::invalid_argument,
                   "set validates names even without overwrite") && ok;
        const auto remove = Env::unset(invalidName);
        ok = check(!remove && remove.error() == std::errc::invalid_argument,
                   "unset rejects invalid names") && ok;
    }

    const std::string invalidValue("prefix\0suffix", 13);
    for (bool overwrite : {false, true}) {
        const auto write = Env::set(name, invalidValue, overwrite);
        ok = check(!write && write.error() == std::errc::invalid_argument,
                   "set rejects embedded NUL without truncating the value") && ok;
    }
    ok = checkValue(name, complexValue) && ok;
    ok = check(Env::unset(name).has_value(), "unset existing value") && ok;
    ok = checkMissing(name) && ok;
    ok = check(std::getenv(name.c_str()) == nullptr, "unset updates the process environment") && ok;

    const auto restored = original->has_value() ? Env::set(name, **original) : Env::unset(name);
    ok = check(restored.has_value(), "restore original environment") && ok;
    return ok ? 0 : 1;
}
