#ifndef GALAY_UTILS_SYSTEM_DETAIL_SYSFS_HPP
#define GALAY_UTILS_SYSTEM_DETAIL_SYSFS_HPP

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <expected>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace galay::utils::detail {

inline std::string_view trimSystemText(std::string_view text)
{
    constexpr std::string_view whitespace = " \t\n\r\f\v";
    const auto first = text.find_first_not_of(whitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(whitespace) - first + 1);
}

// Linux cpulist/nodelist syntax; limit is an exclusive ID bound, never a count
// of online CPUs/nodes. Empty cpulists are valid for CPU-less NUMA nodes.
[[nodiscard]] inline std::expected<std::vector<unsigned>, std::error_code>
parseSystemIds(std::string_view text, unsigned limit, bool allowEmpty = false)
{
    text = trimSystemText(text);
    std::vector<unsigned> ids;
    if (text.empty()) {
        if (allowEmpty) {
            return ids;
        }
        return std::unexpected(std::make_error_code(std::errc::invalid_argument));
    }
    while (!text.empty()) {
        const auto comma = text.find(',');
        const auto item = trimSystemText(text.substr(0, comma));
        const auto dash = item.find('-');
        const auto begin = trimSystemText(item.substr(0, dash));
        if (begin.empty()) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
        unsigned first = 0;
        const auto parsed = std::from_chars(begin.data(), begin.data() + begin.size(), first);
        if (parsed.ec != std::errc{}) {
            return std::unexpected(std::make_error_code(parsed.ec));
        }
        if (parsed.ptr != begin.data() + begin.size()) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
        unsigned last = first;
        if (dash != std::string_view::npos) {
            const auto end = trimSystemText(item.substr(dash + 1));
            if (end.empty()) {
                return std::unexpected(std::make_error_code(std::errc::invalid_argument));
            }
            const auto parsedEnd = std::from_chars(end.data(), end.data() + end.size(), last);
            if (parsedEnd.ec != std::errc{}) {
                return std::unexpected(std::make_error_code(parsedEnd.ec));
            }
            if (parsedEnd.ptr != end.data() + end.size() || last < first) {
                return std::unexpected(std::make_error_code(std::errc::invalid_argument));
            }
        }
        if (first >= limit || last >= limit) {
            return std::unexpected(std::make_error_code(std::errc::value_too_large));
        }
        for (unsigned id = first; ; ++id) {
            ids.push_back(id);
            if (id == last) {
                break;
            }
        }
        if (comma == std::string_view::npos) {
            break;
        }
        text = trimSystemText(text.substr(comma + 1));
        if (text.empty()) {
            return std::unexpected(std::make_error_code(std::errc::invalid_argument));
        }
    }
    std::sort(ids.begin(), ids.end());
    // erase returns a successor iterator; normalization needs no further iteration.
    (void)ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    return ids;
}

#if defined(__linux__)
[[nodiscard]] inline std::expected<std::string, std::error_code>
readSystemText(const std::string& path)
{
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return std::unexpected(std::error_code(errno, std::generic_category()));
    }
    std::string text;
    std::error_code error;
    char buffer[4096];
    for (;;) {
        const auto bytes = read(fd, buffer, sizeof(buffer));
        if (bytes < 0) {
            if (errno == EINTR) {
                continue;
            }
            error = std::error_code(errno, std::generic_category());
            break;
        }
        if (bytes == 0) {
            break;
        }
        // append returns this same string, with no separate result to consume.
        (void)text.append(buffer, static_cast<std::size_t>(bytes));
    }
    // Always check close; a read failure takes precedence over cleanup failure.
    if (close(fd) != 0 && !error) {
        error = std::error_code(errno, std::generic_category());
    }
    if (error) {
        return std::unexpected(error);
    }
    return text;
}

[[nodiscard]] inline std::expected<std::vector<unsigned>, std::error_code>
readSystemIds(const std::string& path, unsigned limit, bool allowEmpty = false)
{
    const auto text = readSystemText(path);
    if (!text) {
        return std::unexpected(text.error());
    }
    return parseSystemIds(*text, limit, allowEmpty);
}
#endif

} // namespace galay::utils::detail

#endif // GALAY_UTILS_SYSTEM_DETAIL_SYSFS_HPP
