#include "../common/benchmark_environment.h"

#include <serde/json/json.hpp>
#include <serde/toml/toml.hpp>
#include <serde/reflect/reflect_macros.hpp>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct Address {
    std::string city;
    std::int32_t zip{};
};
#define ADDRESS_FIELDS(X) X(city) X(zip)
REFLECT_FIELDS(Address, ADDRESS_FIELDS)
#undef ADDRESS_FIELDS

struct User {
    std::string name;
    std::optional<std::string> email;
    std::vector<std::int32_t> scores;
    Address address;
    std::uint64_t id{};
    double score{};
    std::int32_t age{};
    bool active{};
};
#define USER_FIELDS(X) X(name) X(email) X(scores) X(address) X(id) X(score) X(age) X(active)
REFLECT_FIELDS(User, USER_FIELDS)
#undef USER_FIELDS

template <class Operation>
bool measure(std::string_view name, std::size_t iterations, std::size_t bytes,
             Operation&& operation) {
    std::size_t checksum = 0;
    for (std::size_t i = 0; i < 1000; ++i) {
        const auto result = operation();
        if (!result) {
            std::cerr << name << " warmup failed: " << result.error() << '\n';
            return false;
        }
        checksum += *result;
    }
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i = 0; i < iterations; ++i) {
        const auto result = operation();
        if (!result) {
            std::cerr << name << " failed: " << result.error() << '\n';
            return false;
        }
        checksum += *result;
    }
    const auto elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    std::cout << name << " iterations=" << iterations << " payload_bytes=" << bytes
              << " ops_per_second=" << static_cast<double>(iterations) / elapsed
              << " checksum=" << checksum << '\n';
    return true;
}

int main(int argc, char** argv) {
    if (!galay::benchmark::initializeBenchmarkEnvironment()) {
        return 1;
    }

    std::size_t iterations = 20'000;
    if (argc > 2) return 1;
    if (argc == 2) {
        const std::string_view count(argv[1]);
        const auto result = std::from_chars(count.data(), count.data() + count.size(), iterations);
        if (result.ec != std::errc{} || result.ptr != count.data() + count.size() || iterations == 0) {
            std::cerr << "iterations must be a positive integer\n";
            return 1;
        }
    }
    User user{"serde benchmark", "bench@example.com", {1, 2, 3, 5, 8, 13},
              {"Shanghai", 200000}, 7, 3.25, 42, true};
    const auto json_text = json::serialize(user);
    const auto toml_text = toml::serialize(user);
    if (!json_text || !toml_text) return 1;

    bool passed = measure("json.encode", iterations, json_text->size(), [&] {
        ++user.id;
        return json::serialize(user).transform([](const auto& text) { return text.size(); });
    });
    passed &= measure("json.decode", iterations, json_text->size(), [&] {
        return json::deserialize<User>(*json_text).transform([](const auto& value) {
            return value.name.size() + value.scores.size() + value.id;
        });
    });
    passed &= measure("toml.encode", iterations, toml_text->size(), [&] {
        ++user.id;
        return toml::serialize(user).transform([](const auto& text) { return text.size(); });
    });
    passed &= measure("toml.decode", iterations, toml_text->size(), [&] {
        return toml::deserialize<User>(*toml_text).transform([](const auto& value) {
            return value.name.size() + value.scores.size() + value.id;
        });
    });
    return passed ? 0 : 1;
}
