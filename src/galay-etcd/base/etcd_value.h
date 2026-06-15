/**
 * @file etcd_value.h
 * @brief etcd 键值对基础数据结构定义
 * @author galay-etcd
 * @version 1.0.0
 *
 * @details 定义 etcd 中键值对(EtcdKeyValue)的数据结构，
 *          包含键、值以及与版本和租约相关的元数据字段。
 *          该结构被 etcd 客户端的各类操作(Get/Put/Delete/Watch)共同使用。
 */

#ifndef GALAY_ETCD_VALUE_H
#define GALAY_ETCD_VALUE_H

#include <cstdint>
#include <string>

namespace galay::etcd
{

/**
 * @brief etcd 键值对数据结构
 * @details 表示 etcd 中存储的一个完整键值对，包含键、值以及与
 *          版本控制、租约关联等相关的元数据信息。
 */
struct EtcdKeyValue
{
    std::string key;              ///< 键名
    std::string value;            ///< 值内容
    int64_t create_revision = 0;  ///< 键的创建修订版本号
    int64_t mod_revision = 0;     ///< 键的最后修改修订版本号
    int64_t version = 0;          ///< 键在给定修订版本中的版本号
    int64_t lease = 0;            ///< 关联的租约 ID，0 表示无租约
};

} // namespace galay::etcd

#endif // GALAY_ETCD_VALUE_H
