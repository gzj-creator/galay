/**
 * @file t122_logger_slot.cc
 * @brief 用途：验证日志注册槽按库隔离，且 kernel 自身使用 log::set/get 入口。
 */

#include "galay-kernel/common/log_macro.h"
#include "galay-kernel/common/logger.h"

#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace
{

struct FirstLibraryLogTag
{
};

struct SecondLibraryLogTag
{
};

class CollectingLogger final : public galay::kernel::BaseLogger
{
public:
    void log(galay::kernel::LogLevel level,
             std::string_view tag,
             std::string_view message,
             const char* file,
             int line,
             const char* function) override
    {
        m_levels.push_back(level);
        m_tags.emplace_back(tag);
        m_messages.emplace_back(message);
        m_files.emplace_back(file);
        m_lines.push_back(line);
        m_functions.emplace_back(function);
    }

    galay::kernel::LogLevel minLevel() const override
    {
        return m_min_level;
    }

    void setMinLevel(galay::kernel::LogLevel level)
    {
        m_min_level = level;
    }

    size_t size() const
    {
        return m_messages.size();
    }

    const std::string& tagAt(size_t index) const
    {
        return m_tags[index];
    }

    const std::string& messageAt(size_t index) const
    {
        return m_messages[index];
    }

private:
    galay::kernel::LogLevel m_min_level{galay::kernel::LogLevel::kTrace};
    std::vector<galay::kernel::LogLevel> m_levels;
    std::vector<std::string> m_tags;
    std::vector<std::string> m_messages;
    std::vector<std::string> m_files;
    std::vector<int> m_lines;
    std::vector<std::string> m_functions;
};

using FirstSlot = galay::kernel::LoggerSlot<FirstLibraryLogTag>;
using SecondSlot = galay::kernel::LoggerSlot<SecondLibraryLogTag>;

bool expect(bool condition)
{
    return condition;
}

int buildLogValue(int& call_count)
{
    ++call_count;
    return 42;
}

} // namespace

int main()
{
    auto first_logger = std::make_unique<CollectingLogger>();
    auto* first_raw = first_logger.get();
    auto second_logger = std::make_unique<CollectingLogger>();
    auto* second_raw = second_logger.get();
    auto kernel_logger = std::make_unique<CollectingLogger>();
    auto* kernel_raw = kernel_logger.get();

    FirstSlot::set(std::move(first_logger));
    SecondSlot::set(std::move(second_logger));
    galay::kernel::log::set(std::move(kernel_logger));

    GALAY_LOG_WITH_LOGGER(FirstSlot::get,
                          galay::kernel::LogLevel::kInfo,
                          "[first] [event]",
                          "value={}",
                          7);
    GALAY_LOG_WITH_LOGGER(SecondSlot::get,
                          galay::kernel::LogLevel::kWarn,
                          "[second] [event]",
                          "state={}",
                          "warn");
    GALAY_KERNEL_LOG_ERROR("[kernel] [event]", "code={}", 42);

    if (!expect(first_raw->size() == 1) ||
        !expect(first_raw->tagAt(0) == "[first] [event]") ||
        !expect(first_raw->messageAt(0) == "value=7")) {
        return EXIT_FAILURE;
    }

    if (!expect(second_raw->size() == 1) ||
        !expect(second_raw->tagAt(0) == "[second] [event]") ||
        !expect(second_raw->messageAt(0) == "state=warn")) {
        return EXIT_FAILURE;
    }

    if (!expect(kernel_raw->size() == 1) ||
        !expect(kernel_raw->tagAt(0) == "[kernel] [event]") ||
        !expect(kernel_raw->messageAt(0) == "code=42")) {
        return EXIT_FAILURE;
    }

    int disabled_argument_count = 0;
    FirstSlot::set(nullptr);
    GALAY_LOG_WITH_LOGGER(FirstSlot::get,
                          galay::kernel::LogLevel::kError,
                          "[first] [disabled]",
                          "value={}",
                          buildLogValue(disabled_argument_count));
    if (!expect(disabled_argument_count == 0)) {
        return EXIT_FAILURE;
    }

    auto filtered_logger = std::make_unique<CollectingLogger>();
    auto* filtered_raw = filtered_logger.get();
    filtered_raw->setMinLevel(galay::kernel::LogLevel::kError);
    FirstSlot::set(std::move(filtered_logger));

    int filtered_argument_count = 0;
    GALAY_LOG_WITH_LOGGER(FirstSlot::get,
                          galay::kernel::LogLevel::kDebug,
                          "[first] [filtered]",
                          "value={}",
                          buildLogValue(filtered_argument_count));
    if (!expect(filtered_argument_count == 0) || !expect(filtered_raw->size() == 0)) {
        return EXIT_FAILURE;
    }

    int enabled_argument_count = 0;
    if (GALAY_LOG_ENABLED(FirstSlot::get, galay::kernel::LogLevel::kError)) {
        GALAY_LOG_WITH_LOGGER(FirstSlot::get,
                              galay::kernel::LogLevel::kError,
                              "[first] [enabled]",
                              "value={}",
                              buildLogValue(enabled_argument_count));
    }
    if (!expect(enabled_argument_count == 1) ||
        !expect(filtered_raw->size() == 1) ||
        !expect(filtered_raw->messageAt(0) == "value=42")) {
        return EXIT_FAILURE;
    }

    FirstSlot::set(nullptr);
    SecondSlot::set(nullptr);
    galay::kernel::log::set(nullptr);
    return EXIT_SUCCESS;
}
