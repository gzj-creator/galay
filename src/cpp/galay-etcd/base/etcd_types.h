/**
 * @file etcd_types.h
 * @brief etcd 操作类型、Pipeline 结构和 Watch 事件类型定义
 * @author galay-etcd
 * @version 1.0.0
 *
 * @details 定义 etcd 客户端使用的核心类型，包括：
 *          - Pipeline 操作类型(PipelineOpType)与操作描述(PipelineOp)
 *          - Pipeline 单条操作结果(PipelineItemResult)
 *          - Watch 事件类型(EtcdWatchEventType)、事件(EtcdWatchEvent)和响应(EtcdWatchResponse)
 *          - 各类操作的返回值类型别名(EtcdBoolResult、EtcdGetResult 等)
 */

#ifndef GALAY_ETCD_TYPES_H
#define GALAY_ETCD_TYPES_H

#include "etcd_error.h"
#include "etcd_value.h"

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace galay::etcd
{

/**
 * @brief Pipeline 操作类型枚举
 * @details 描述 Pipeline 事务中单个操作的类型
 */
enum class PipelineOpType
{
    Put,    ///< 写入键值对
    Get,    ///< 读取键值对
    Delete, ///< 删除键值对
};

/**
 * @brief Pipeline 操作描述
 * @details 描述 Pipeline 事务中的一个原子操作，
 *          包括操作类型、目标键值、是否前缀匹配以及可选的租约 ID。
 */
struct PipelineOp
{
    PipelineOpType type = PipelineOpType::Put;  ///< 操作类型
    std::string key;                             ///< 目标键
    std::string value;                           ///< 写入的值（仅 Put 操作使用）
    bool prefix = false;                         ///< 是否为前缀匹配（Get/Delete 操作使用）
    std::optional<int64_t> limit = std::nullopt; ///< 返回数量限制（Get 操作使用）
    std::optional<int64_t> lease_id = std::nullopt; ///< 租约 ID（Put 操作使用）

    /**
     * @brief 创建一个 Put 操作
     * @param key 键名
     * @param value 值
     * @param lease_id 可选的租约 ID
     * @return Put 类型的 PipelineOp
     */
    static PipelineOp Put(std::string key,
                          std::string value,
                          std::optional<int64_t> lease_id = std::nullopt)
    {
        PipelineOp op;
        op.type = PipelineOpType::Put;
        op.key = std::move(key);
        op.value = std::move(value);
        op.lease_id = lease_id;
        return op;
    }

    /**
     * @brief 创建一个 Get 操作
     * @param key 键名
     * @param prefix 是否为前缀查询
     * @param limit 返回数量限制
     * @return Get 类型的 PipelineOp
     */
    static PipelineOp Get(std::string key,
                          bool prefix = false,
                          std::optional<int64_t> limit = std::nullopt)
    {
        PipelineOp op;
        op.type = PipelineOpType::Get;
        op.key = std::move(key);
        op.prefix = prefix;
        op.limit = limit;
        return op;
    }

    /**
     * @brief 创建一个 Delete 操作
     * @param key 键名
     * @param prefix 是否为前缀删除
     * @return Delete 类型的 PipelineOp
     */
    static PipelineOp Del(std::string key, bool prefix = false)
    {
        PipelineOp op;
        op.type = PipelineOpType::Delete;
        op.key = std::move(key);
        op.prefix = prefix;
        return op;
    }
};

/**
 * @brief Pipeline 单条操作的结果
 * @details 描述 Pipeline 事务中一个操作的执行结果，
 *          根据操作类型(Put/Get/Delete)包含不同的结果数据。
 */
struct PipelineItemResult
{
    PipelineOpType type = PipelineOpType::Put; ///< 对应的操作类型
    bool ok = false;                           ///< 操作是否成功
    int64_t deleted_count = 0;                 ///< 删除的键数量（Delete 操作）
    std::vector<EtcdKeyValue> kvs;             ///< 查询到的键值对列表（Get 操作）
};

/**
 * @brief Watch 事件类型枚举
 * @details 描述 Watch 回调中事件的变更类型
 */
enum class EtcdWatchEventType
{
    Put,    ///< 键被写入或更新
    Delete, ///< 键被删除
    Unknown,///< 未知事件类型
};

/**
 * @brief 单条 watch 事件
 * @note `prev_kv` 只有服务端返回时才有值。
 */
struct EtcdWatchEvent
{
    EtcdWatchEventType type = EtcdWatchEventType::Unknown; ///< 事件类型
    EtcdKeyValue kv;                                       ///< 事件对应的键值对
    std::optional<EtcdKeyValue> prev_kv = std::nullopt;    ///< 变更前的键值对（可选）
};

/**
 * @brief 一次 watch 回调批次的结果
 * @note etcd 可能先返回 `created=true` 且 `events` 为空的创建确认，再返回实际事件批次。
 */
struct EtcdWatchResponse
{
    int64_t watch_id = 0;                   ///< Watch ID
    bool created = false;                   ///< 是否为创建确认
    bool canceled = false;                  ///< 是否已被取消
    int64_t compact_revision = 0;           ///< 压缩修订版本号
    std::vector<EtcdWatchEvent> events;     ///< 事件列表
};

using EtcdVoidResult = std::expected<void, EtcdError>;                              ///< 无返回值操作结果
using EtcdBoolResult = std::expected<bool, EtcdError>;                              ///< 布尔操作结果
using EtcdGetResult = std::expected<std::vector<EtcdKeyValue>, EtcdError>;          ///< Get 操作结果
using EtcdDeleteResult = std::expected<int64_t, EtcdError>;                         ///< Delete 操作结果（返回删除数量）
using EtcdLeaseGrantResult = std::expected<int64_t, EtcdError>;                     ///< 租约操作结果（返回租约 ID）
using EtcdPipelineResult = std::expected<std::vector<PipelineItemResult>, EtcdError>; ///< Pipeline 操作结果

} // namespace galay::etcd

#endif // GALAY_ETCD_TYPES_H
