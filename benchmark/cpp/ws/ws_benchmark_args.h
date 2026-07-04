/**
 * @file ws_benchmark_args.h
 * @brief WS benchmark 命令行参数辅助函数。
 */

#ifndef GALAY_BENCHMARK_CPP_WS_BENCHMARK_ARGS_H
#define GALAY_BENCHMARK_CPP_WS_BENCHMARK_ARGS_H

#include <cctype>
#include <string>
#include <string_view>

namespace galay::benchmark::ws {

inline constexpr std::string_view kDefaultBenchmarkClientUrl = "ws://127.0.0.1:8080/ws";

/**
 * @brief 解析 WS client benchmark 的目标 URL。
 * @param argc main() argc。
 * @param argv main() argv，前 3 个业务参数仍为 clients/duration/message_size。
 * @return 第 4 个业务参数存在且非空时返回该 URL，否则返回默认 8080 endpoint。
 */
inline std::string resolveBenchmarkClientUrl(int argc, char* argv[])
{
    if (argc > 4 && argv[4] != nullptr && argv[4][0] != '\0') {
        return argv[4];
    }
    return std::string(kDefaultBenchmarkClientUrl);
}

namespace detail {

inline std::string toLowerAscii(std::string_view value)
{
    std::string lowered;
    lowered.reserve(value.size());
    for (char ch : value) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    return lowered;
}

} // namespace detail

/**
 * @brief 解析 server benchmark 的 TCP_NODELAY 开关。
 * @param argc main() argc。
 * @param argv main() argv。
 * @param arg_index nodelay 参数所在 argv 下标。
 * @return 未传、空值或无法识别时默认返回 true；off/false/0/no/disable/disabled 返回 false。
 */
inline bool resolveBenchmarkServerNoDelay(int argc, char* argv[], int arg_index)
{
    if (arg_index < 0 || argc <= arg_index || argv[arg_index] == nullptr || argv[arg_index][0] == '\0') {
        return true;
    }

    const std::string value = detail::toLowerAscii(argv[arg_index]);
    if (value == "0" || value == "false" || value == "off" ||
        value == "no" || value == "disable" || value == "disabled") {
        return false;
    }
    return true;
}

} // namespace galay::benchmark::ws

#endif // GALAY_BENCHMARK_CPP_WS_BENCHMARK_ARGS_H
