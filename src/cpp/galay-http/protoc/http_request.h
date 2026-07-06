/**
 * @file http_request.h
 * @brief HTTP 请求消息定义
 * @author galay-http
 * @version 1.0.0
 *
 * @details 定义 HttpRequest 类，封装完整的 HTTP 请求消息，
 *          包括请求头、请求体、路由参数，支持增量解析。
 */

#ifndef GALAY_HTTP_REQUEST_H
#define GALAY_HTTP_REQUEST_H

#include "http_header.h"
#include "http_body.h"
#include "http_chunk.h"
#include <vector>
#include <sys/uio.h>

namespace galay::http
{

/**
 * @brief HTTP 请求消息
 * @details 封装 HTTP 请求头与请求体，提供序列化、增量解析及路由参数管理。
 */
class HttpRequest
{
public:
    using ptr = std::shared_ptr<HttpRequest>;   ///< 共享指针类型别名
    using wptr = std::weak_ptr<HttpRequest>;    ///< 弱指针类型别名
    using uptr = std::unique_ptr<HttpRequest>;  ///< 独占指针类型别名

    HttpRequest() = default;
    HttpRequest(HttpRequest&&) noexcept = default;   ///< 移动构造
    HttpRequest& operator=(HttpRequest&&) noexcept = default; ///< 移动赋值
    ~HttpRequest() = default;

    /**
     * @brief 获取请求头的可变引用
     * @return 请求头引用
     */
    HttpRequestHeader& header();

    /**
     * @brief 获取请求体（模板版，移交所有权）
     * @tparam T Body 类型，须满足 HttpBodyType concept
     * @return 解析后的 Body 对象
     */
    template<HttpBodyType T>
    T getBody();

    /**
     * @brief 获取请求体原始字符串（破坏性操作，会清空 body）
     * @return Body 字符串
     */
    std::string getBodyStr();

    /**
     * @brief 获取请求体的常量引用（非破坏性，推荐用于读取）
     * @return Body 字符串的常量引用
     */
    const std::string& bodyStr() const;

    /**
     * @brief 设置请求头（移动语义）
     * @param header 请求头
     */
    void setHeader(HttpRequestHeader&& header);

    /**
     * @brief 设置请求头（左值引用）
     * @param header 请求头
     */
    void setHeader(HttpRequestHeader& header);

    /**
     * @brief 设置请求体（模板版，移动语义）
     * @tparam T Body 类型
     * @param body Body 对象
     */
    template<HttpBodyType T>
    void setBody(T&& body);

    /**
     * @brief 设置请求体原始字符串
     * @param body Body 数据（移动语义）
     */
    void setBodyStr(std::string&& body);

    /**
     * @brief 将请求序列化为字符串
     * @return 完整的 HTTP 请求报文字符串
     */
    std::string toString();

    /**
     * @brief 从 iovec 数组增量解析请求
     * @param iovecs 离散缓冲区数组
     * @return pair.first 为错误码，pair.second 为消耗的字节数（-1 错误，0 不完整）
     */
    std::pair<HttpErrorCode, ssize_t> fromIOVec(const std::vector<iovec>& iovecs);

    /**
     * @brief 从 iovec 数组增量解析请求，并检查 body 大小限制
     * @param iovecs 离散缓冲区数组
     * @param max_body_size 最大 body 字节数，0 表示不限制
     * @return pair.first 为错误码，pair.second 为消耗的字节数（-1 错误，0 不完整）
     */
    std::pair<HttpErrorCode, ssize_t> fromIOVec(const std::vector<iovec>& iovecs,
                                                size_t max_body_size);

    /**
     * @brief 检查请求是否解析完成（header + body）
     * @return 解析完成返回 true
     */
    bool isComplete() const;

    void reset(); ///< 重置解析状态

    // ==================== 路由参数支持 ====================
    /**
     * @brief 设置路由参数
     * @param params 路径参数映射（例如 /user/:id 中的 id -> 123）
     */
    void setRouteParams(std::map<std::string, std::string>&& params);

    /**
     * @brief 获取所有路由参数
     * @return 路由参数映射
     */
    const std::map<std::string, std::string>& routeParams() const;

    /**
     * @brief 获取指定的路由参数
     * @param name 参数名
     * @param defaultValue 默认值（参数不存在时返回）
     * @return 参数值
     */
    std::string getRouteParam(const std::string& name, const std::string& defaultValue = "") const;

    /**
     * @brief 检查是否存在指定的路由参数
     * @param name 参数名
     * @return 是否存在
     */
    bool hasRouteParam(const std::string& name) const;

private:
    std::string m_body;                    ///< 请求体原始数据
    HttpRequestHeader m_header;            ///< 请求头
    size_t m_contentLength = 0;            ///< Content-Length 值
    size_t m_bodyParsed = 0;               ///< 已解析的 body 字节数
    size_t m_headerLength = 0;             ///< header 的字节长度
    ChunkParser m_chunkParser;             ///< chunked body 增量解析状态
    std::map<std::string, std::string> m_routeParams; ///< 路由参数（由 HttpRouter 设置）
    bool m_headerParsed = false;           ///< header 是否已解析完成
};

}

#include "http_request.inl"

#endif
