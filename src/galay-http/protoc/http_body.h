/**
 * @file http_body.h
 * @brief HTTP 请求/响应体抽象接口
 * @author galay-http
 * @version 1.0.0
 *
 * @details 定义 HTTP 消息体（Body）的抽象基类、纯文本实现以及
 *          用于约束合法 Body 类型的 concept 模板。
 */

#ifndef GALAY_HTTP_BODY_H
#define GALAY_HTTP_BODY_H

#include "http_base.h"
#include <concepts>

namespace galay::http
{

/**
 * @brief HTTP 消息体抽象基类
 * @details 提供统一的 Body 序列化/反序列化接口，所有具体的 Body 类型均需继承此类。
 */
class HttpBody {
public:
    virtual ~HttpBody() = default;

    /**
     * @brief 获取 Content-Type
     * @return MIME 类型字符串，如 "text/plain"、"application/json"
     */
    virtual std::string contentType() = 0;

    /**
     * @brief 从字符串填充 Body 内容（移动语义）
     * @param str 包含 Body 数据的字符串，调用后原始字符串不再有效
     * @return 解析成功返回 true，否则返回 false
     */
    virtual bool fromString(std::string&& str) = 0;

    /**
     * @brief 将 Body 内容序列化为字符串（移动语义，会清空内部数据）
     * @return 包含 Body 数据的字符串
     */
    virtual std::string toString() = 0;
};

/**
 * @brief 纯文本 Body 实现
 * @details 用于 text/plain 类型的请求/响应体
 */
class PlainBody: public HttpBody
{
public:
    std::string contentType() override { return "text/plain"; } ///< 返回 "text/plain"

    /**
     * @brief 从字符串填充 Body 内容
     * @param str Body 数据（移动语义）
     * @return 始终返回 true
     */
    bool fromString(std::string&& str) override;

    /**
     * @brief 将 Body 内容序列化为字符串（移交所有权，破坏性操作）
     * @return 包含 Body 数据的字符串
     */
    std::string toString() override;

private:
    std::string m_body; ///< Body 数据
};



/**
 * @brief 合法 HTTP Body 类型约束
 * @details 要求类型 T 必须继承自 HttpBody、可默认构造、可移动赋值和移动构造。
 * @tparam T 待检查的 Body 类型
 */
template<typename T>
concept HttpBodyType =
    std::derived_from<T, HttpBody> &&
    std::is_default_constructible_v<T> &&
    std::is_move_assignable_v<T> &&
    std::is_move_constructible_v<T>;

}

#endif