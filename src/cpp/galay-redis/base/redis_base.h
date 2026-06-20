/**
 * @file redis_base.h
 * @brief Redis 基础类型约束定义
 * @author galay-redis
 * @version 1.0.0
 *
 * @details 定义 Redis 客户端接口所需的 C++20 concept 类型约束，
 *          包括键值对、键、值和分值等类型要求。
 */

#ifndef GALAY_REDIS_BASE_H
#define GALAY_REDIS_BASE_H

#include <concepts>
#include <string>
#include <cstdint>

namespace galay::redis
{
    /**
     * @brief 键值对类型约束
     * @tparam T 待检查的类型
     * @details 要求类型为 std::pair<std::string, std::string>
     */
    template <typename T>
    concept KVPair = std::same_as<T, std::pair<std::string, std::string>>;

    /**
     * @brief 键类型约束
     * @tparam T 待检查的类型
     * @details 要求类型为 std::string
     */
    template <typename T>
    concept KeyType = std::same_as<T, std::string>;

    /**
     * @brief 值类型约束
     * @tparam T 待检查的类型
     * @details 要求类型为 std::string、int64_t 或 double
     */
    template <typename T>
    concept ValType = std::same_as<T, std::string> || std::same_as<T, int64_t> || std::same_as<T, double>;

    /**
     * @brief 分数值类型约束
     * @tparam T 待检查的类型
     * @details 要求类型为 std::pair<double, std::string>，用于有序集合操作
     */
    template <typename T>
    concept ScoreValType = std::same_as<T, std::pair<double, std::string>>;

}


#endif