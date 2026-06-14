#ifndef GALAY_TEST_STDOUT_LOG_H
#define GALAY_TEST_STDOUT_LOG_H

#include <iostream>
#include <utility>

namespace galay::test::stdoutlog
{
namespace detail
{
inline void writeLine(std::ostream &os)
{
    os << '\n';
}

template<typename First, typename... Rest>
inline void writeLine(std::ostream &os, First&& first, Rest&&... rest)
{
    os << std::forward<First>(first);
    ((os << ' ' << std::forward<Rest>(rest)), ...);
    os << '\n';
}
} // namespace detail

template<typename... Args>
inline void trace(Args&&... args)
{
    detail::writeLine(std::cout, std::forward<Args>(args)...);
}

template<typename... Args>
inline void debug(Args&&... args)
{
    detail::writeLine(std::cout, std::forward<Args>(args)...);
}

template<typename... Args>
inline void info(Args&&... args)
{
    detail::writeLine(std::cout, std::forward<Args>(args)...);
}

template<typename... Args>
inline void warn(Args&&... args)
{
    detail::writeLine(std::cout, std::forward<Args>(args)...);
}

template<typename... Args>
inline void error(Args&&... args)
{
    detail::writeLine(std::cerr, std::forward<Args>(args)...);
}

template<typename... Args>
inline void critical(Args&&... args)
{
    detail::writeLine(std::cerr, std::forward<Args>(args)...);
}
} // namespace galay::test::stdoutlog

#define LogTrace(...)    ::galay::test::stdoutlog::trace(__VA_ARGS__)
#define LogDebug(...)    ::galay::test::stdoutlog::debug(__VA_ARGS__)
#define LogInfo(...)     ::galay::test::stdoutlog::info(__VA_ARGS__)
#define LogWarn(...)     ::galay::test::stdoutlog::warn(__VA_ARGS__)
#define LogError(...)    ::galay::test::stdoutlog::error(__VA_ARGS__)
#define LogCritical(...) ::galay::test::stdoutlog::critical(__VA_ARGS__)

#endif
