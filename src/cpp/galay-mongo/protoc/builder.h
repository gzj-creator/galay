#ifndef GALAY_MONGO_PROTOCOL_BUILDER_H
#define GALAY_MONGO_PROTOCOL_BUILDER_H

#include "mongo_protocol.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace galay::mongo::protocol
{

class MongoCommandBuilder
{
public:
    MongoCommandBuilder() = default;
    MongoCommandBuilder(MongoCommandBuilder&&) noexcept = default;             ///< 移动构造，转移命令列表所有权
    MongoCommandBuilder& operator=(MongoCommandBuilder&&) noexcept = default;  ///< 移动赋值，转移命令列表所有权

    /**
     * @brief 显式深拷贝命令列表
     * @return 每个命令递归 clone 后的新 builder
     */
    [[nodiscard]] MongoCommandBuilder clone() const;

    void clear() noexcept;
    void reserve(size_t command_count);

    MongoCommandBuilder& append(MongoDocument command);
    MongoCommandBuilder& append(std::string_view command_name,
                                MongoValue command_value,
                                MongoDocument arguments = {});
    MongoCommandBuilder& appendPing();

    [[nodiscard]] std::span<const MongoDocument> commands() const noexcept;
    [[nodiscard]] size_t size() const noexcept;
    [[nodiscard]] bool empty() const noexcept;

    [[nodiscard]] std::expected<std::string, std::string> encodePipeline(std::string_view database,
                                                                         int32_t first_request_id,
                                                                         size_t reserve_per_command = 96) const;
    [[nodiscard]] static std::expected<std::string, std::string> encodePipeline(std::string_view database,
                                                                                int32_t first_request_id,
                                                                                std::span<const MongoDocument> commands,
                                                                                size_t reserve_per_command = 96);

private:
    MongoCommandBuilder(const MongoCommandBuilder&) = delete;
    MongoCommandBuilder& operator=(const MongoCommandBuilder&) = delete;

    std::vector<MongoDocument> m_commands;
};

} // namespace galay::mongo::protocol

#endif // GALAY_MONGO_PROTOCOL_BUILDER_H
