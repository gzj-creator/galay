/**
 * @file t6_rpc_log.cpp
 * @brief 验证 galay-rpc 库级日志入口与空 logger 过滤路径。
 */

#include "galay-rpc/common/rpc_log.h"

#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace
{

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

int buildLogValue(int& call_count)
{
    ++call_count;
    return 42;
}

} // namespace

int main()
{
    galay::rpc::log::set(nullptr);

    int disabled_argument_count = 0;
    RPC_LOG_ERROR("[disabled]", "value={}", buildLogValue(disabled_argument_count));
    if (disabled_argument_count != 0 || RPC_LOG_ENABLED(galay::kernel::LogLevel::kError)) {
        return EXIT_FAILURE;
    }

    auto logger = std::make_unique<CollectingLogger>();
    auto* raw_logger = logger.get();
    raw_logger->setMinLevel(galay::kernel::LogLevel::kWarn);
    galay::rpc::log::set(std::move(logger));

    int filtered_argument_count = 0;
    RPC_LOG_DEBUG("[filtered]", "value={}", buildLogValue(filtered_argument_count));
    if (filtered_argument_count != 0 || raw_logger->size() != 0) {
        return EXIT_FAILURE;
    }

    RPC_LOG_WARN("[enabled]", "value={}", buildLogValue(filtered_argument_count));
    if (filtered_argument_count != 1 ||
        raw_logger->size() != 1 ||
        raw_logger->tagAt(0) != "[rpc] [enabled]" ||
        raw_logger->messageAt(0) != "value=42") {
        return EXIT_FAILURE;
    }

    galay::rpc::log::set(nullptr);
    return EXIT_SUCCESS;
}
