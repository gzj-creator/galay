#include <galay/cpp/galay-redis/protoc/redis_protocol.h>

#include <array>
#include <iostream>
#include <span>
#include <string_view>
#include <vector>

using namespace galay::redis::protocol;

namespace
{
    bool check_estimated_size(std::string_view command, std::span<const std::string_view> args)
    {
        RespEncoder encoder;
        std::string encoded;
        encoder.append(encoded, command, args);

        const size_t estimated = encoder.estimate_command_bytes(command, args);
        if (estimated != encoded.size()) {
            std::cerr << "estimate mismatch for command " << command
                      << ": estimated=" << estimated
                      << ", actual=" << encoded.size() << "\n";
            return false;
        }
        return true;
    }
}

int main()
{
    const std::array<std::string_view, 0> no_args{};
    const std::array<std::string_view, 1> get_args{"bench:key"};
    const std::array<std::string_view, 2> set_args{"bench:key", "bench:value"};
    const std::array<std::string_view, 3> hmset_args{"bench:key", "field", "value"};

    if (!check_estimated_size("PING", no_args)) {
        return 1;
    }
    if (!check_estimated_size("GET", get_args)) {
        return 1;
    }
    if (!check_estimated_size("SET", set_args)) {
        return 1;
    }
    if (!check_estimated_size("HMSET", hmset_args)) {
        return 1;
    }

    std::vector<std::string_view> dynamic_args{
        "bench:key",
        "42",
        "PX",
        "1500",
        "NX",
    };
    if (!check_estimated_size("SET", dynamic_args)) {
        return 1;
    }

    std::cout << "T20-RespEncoderEstimate PASS\n";
    return 0;
}
