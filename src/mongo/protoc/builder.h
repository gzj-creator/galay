#ifndef GALAY_MONGO_PROTOCOL_BUILDER_H
#define GALAY_MONGO_PROTOCOL_BUILDER_H

#include "mongo_protocol.h"

#include <cstddef>
#include <cstdint>
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

    [[nodiscard]] std::string encodePipeline(std::string_view database,
                                             int32_t first_request_id,
                                             size_t reserve_per_command = 96) const;
    [[nodiscard]] static std::string encodePipeline(std::string_view database,
                                                    int32_t first_request_id,
                                                    std::span<const MongoDocument> commands,
                                                    size_t reserve_per_command = 96);

private:
    std::vector<MongoDocument> m_commands;
};

} // namespace galay::mongo::protocol

#endif // GALAY_MONGO_PROTOCOL_BUILDER_H
