/**
 * @file http_response.h
 * @brief HTTP 响应消息定义
 * @author galay-http
 * @version 1.0.0
 *
 * @details 定义 HttpResponse 类，封装完整的 HTTP 响应消息，
 *          包括响应头、响应体，支持增量解析。
 */

#ifndef GALAY_HTTP_RESPONSE_H
#define GALAY_HTTP_RESPONSE_H

#include "http_header.h"
#include "http_body.h"
#include "http_chunk.h"
#include <vector>
#include <sys/uio.h>

namespace galay::http
{

/**
 * @brief HTTP 响应消息
 * @details 封装 HTTP 响应头与响应体，提供序列化与增量解析。
 */
class HttpResponse
{
public:
    using ptr = std::shared_ptr<HttpResponse>;   ///< 共享指针类型别名
    using wptr = std::weak_ptr<HttpResponse>;    ///< 弱指针类型别名
    using uptr = std::unique_ptr<HttpResponse>;  ///< 独占指针类型别名

    HttpResponse() = default;
    HttpResponse(HttpResponse&&) noexcept = default;   ///< 移动构造
    HttpResponse& operator=(HttpResponse&&) noexcept = default; ///< 移动赋值
    ~HttpResponse() = default;

    /**
     * @brief 显式复制响应消息及其解析状态
     * @return 独立的 HttpResponse 副本
     */
    HttpResponse clone() const;

    /**
     * @brief 获取响应头的可变引用
     * @return 响应头引用
     */
    HttpResponseHeader& header();

    /**
     * @brief 获取响应体（模板版，移交所有权）
     * @tparam T Body 类型，须满足 HttpBodyType concept
     * @return 解析后的 Body 对象
     */
    template<HttpBodyType T>
    T getBody();

    /**
     * @brief 获取响应体原始字符串（破坏性操作，会清空 body）
     * @return Body 字符串
     */
    std::string getBodyStr();

    /**
     * @brief 获取响应体的常量引用（非破坏性，推荐用于读取）
     * @return Body 字符串的常量引用
     */
    const std::string& bodyStr() const;

    /**
     * @brief 设置响应头（移动语义）
     * @param header 响应头
     */
    void setHeader(HttpResponseHeader&& header);

    /**
     * @brief 设置响应头（左值引用）
     * @param header 响应头
     */
    void setHeader(HttpResponseHeader& header);

    /**
     * @brief 设置响应体（模板版，移动语义）
     * @tparam T Body 类型
     * @param body Body 对象
     */
    template<HttpBodyType T>
    void setBody(T&& body);

    /**
     * @brief 设置响应体原始字符串
     * @param body Body 数据（移动语义）
     */
    void setBodyStr(std::string&& body);

    /**
     * @brief 将响应序列化为字符串
     * @return 完整的 HTTP 响应报文字符串
     */
    std::string toString();

    /**
     * @brief 从 iovec 数组增量解析响应
     * @param iovecs 离散缓冲区数组
     * @return pair.first 为错误码，pair.second 为消耗的字节数（-1 错误，0 不完整）
     */
    std::pair<HttpErrorCode, ssize_t> fromIOVec(const std::vector<iovec>& iovecs);

    /**
     * @brief 从 iovec 数组增量解析响应，并检查 body 大小限制
     * @param iovecs 离散缓冲区数组
     * @param max_body_size 最大 body 字节数，0 表示不限制
     * @return pair.first 为错误码，pair.second 为消耗的字节数（-1 错误，0 不完整）
     */
    std::pair<HttpErrorCode, ssize_t> fromIOVec(const std::vector<iovec>& iovecs,
                                                size_t max_body_size);

    /**
     * @brief 检查响应是否解析完成（header + body）
     * @return 解析完成返回 true
     */
    bool isComplete() const;

    void reset(); ///< 重置解析状态

private:
    HttpResponse(const HttpResponse&) = delete;
    HttpResponse& operator=(const HttpResponse&) = delete;

    HttpResponseHeader m_header;          ///< 响应头
    std::string m_body;                   ///< 响应体原始数据
    size_t m_contentLength = 0;           ///< Content-Length 值
    size_t m_bodyParsed = 0;              ///< 已解析的 body 字节数
    size_t m_headerLength = 0;            ///< header 的字节长度
    ChunkParser m_chunkParser;            ///< chunked body 增量解析状态
    bool m_headerParsed = false;          ///< header 是否已解析完成
};

}


#include "http_response.inl"

#endif
