/**
 * @file mongo_uri.h
 * @brief MongoDB URI 解析
 * @author galay-mongo
 * @version 1.0.0
 *
 * @details 将 mongodb:// URI 转换为 MongoConfig。解析失败通过 std::expected
 * 返回 MongoError，不抛出异常。
 */

#ifndef GALAY_MONGO_URI_H
#define GALAY_MONGO_URI_H

#include "mongo_config.h"
#include "mongo_error.h"

#include <expected>
#include <string_view>

namespace galay::mongo
{

/**
 * @brief 解析 MongoDB URI
 * @param uri 形如 mongodb://host1,host2/db?replicaSet=rs0 的 URI
 * @return 成功返回 MongoConfig；失败返回 MONGO_ERROR_INVALID_PARAM 或 MONGO_ERROR_UNSUPPORTED
 * @note 当前仅支持 mongodb:// scheme。TLS URI 选项在 TLS task 落地前返回不支持。
 */
std::expected<MongoConfig, MongoError> parseMongoUri(std::string_view uri);

} // namespace galay::mongo

#endif // GALAY_MONGO_URI_H
