/**
 * @file http_header.h
 * @brief HTTP 头部解析与存储
 * @author galay-http
 * @version 1.0.0
 *
 * @details 定义 HTTP 请求头与响应头的解析状态机、Header 键值对存储，
 *          以及基于 fast-path 优化的常见 Header 索引机制。
 */

#ifndef GALAY_HTTP_HEADER_H
#define GALAY_HTTP_HEADER_H

#include "http_base.h"
#include "http_error.h"
#include <string_view>
#include <map>
#include <memory>
#include <vector>
#include <sys/uio.h>
#include <array>
#include <bitset>
#include <functional>


namespace galay::http {

    /**
     * @brief 常见 HTTP Header 索引（用于 fast-path 优化）
     * @details 在服务端模式下，高频 Header 存储在固定大小数组中，
     *          通过索引直接访问，避免 map 查找开销。
     */
    enum class CommonHeaderIndex : uint8_t {
        Host = 0,           ///< Host 头
        ContentLength,      ///< Content-Length 头
        ContentType,        ///< Content-Type 头
        UserAgent,          ///< User-Agent 头
        Accept,             ///< Accept 头
        AcceptEncoding,     ///< Accept-Encoding 头
        Connection,         ///< Connection 头
        CacheControl,       ///< Cache-Control 头
        Cookie,             ///< Cookie 头
        Authorization,      ///< Authorization 头
        IfModifiedSince,    ///< If-Modified-Since 头
        IfNoneMatch,        ///< If-None-Match 头
        Referer,            ///< Referer 头
        AcceptLanguage,     ///< Accept-Language 头
        Range,              ///< Range 头
        NotCommon = 255     ///< 非常见 Header 标记
    };

    /**
     * @brief HTTP 请求头增量解析状态
     * @details 状态机枚举，用于逐字符解析请求行与头部字段
     */
    enum class RequestParseState {
        Method,         ///< 正在解析请求方法
        MethodSP,       ///< 方法后的空格
        Uri,            ///< 正在解析 URI
        UriSP,          ///< URI 后的空格
        Version,        ///< 正在解析 HTTP 版本
        VersionCR,      ///< 版本后的回车符
        VersionLF,      ///< 版本后的换行符
        HeaderKey,      ///< 正在解析头部键名
        HeaderColon,    ///< 头部键名后的冒号
        HeaderSpace,    ///< 冒号后的空格
        HeaderValue,    ///< 正在解析头部值
        HeaderCR,       ///< 头部值后的回车符
        HeaderLF,       ///< 头部值后的换行符
        HeaderEndCR,    ///< 头部结束标记的回车符
        Done            ///< 解析完成
    };

    /**
     * @brief HTTP 响应头增量解析状态
     * @details 状态机枚举，用于逐字符解析状态行与头部字段
     */
    enum class ResponseParseState {
        Version,        ///< 正在解析 HTTP 版本
        VersionSP,      ///< 版本后的空格
        Code,           ///< 正在解析状态码
        CodeSP,         ///< 状态码后的空格
        Status,         ///< 正在解析状态描述
        StatusCR,       ///< 状态描述后的回车符
        StatusLF,       ///< 状态描述后的换行符
        HeaderKey,      ///< 正在解析头部键名
        HeaderColon,    ///< 头部键名后的冒号
        HeaderSpace,    ///< 冒号后的空格
        HeaderValue,    ///< 正在解析头部值
        HeaderCR,       ///< 头部值后的回车符
        HeaderLF,       ///< 头部值后的换行符
        HeaderEndCR,    ///< 头部结束标记的回车符
        Done            ///< 解析完成
    };

    /**
     * @brief HTTP 头部键值对存储
     * @details 支持两种模式：服务端模式（fast-path + 统一小写键名）
     *          和客户端模式（保留原始大小写、仅使用 slow-path map）。
     */
    class HeaderPair
    {
    public:
        /**
         * @brief 头部存储模式
         */
        enum class Mode {
            ServerSide,   ///< 服务端：统一小写，使用 fast-path
            ClientSide    ///< 客户端：保留原始大小写，不使用 fast-path
        };

        /**
         * @brief 构造 HeaderPair
         * @param mode 存储模式，默认为服务端模式
         */
        explicit HeaderPair(Mode mode = Mode::ServerSide);
        HeaderPair(const HeaderPair& other); ///< 拷贝构造
        HeaderPair(HeaderPair&& other);      ///< 移动构造

        /**
         * @brief 判断指定键名是否存在
         * @param key 头部键名
         * @return 存在返回 true
         */
        bool hasKey(const std::string& key) const;

        /**
         * @brief 获取指定键名的值
         * @param key 头部键名
         * @return 对应的值字符串，不存在时返回空串
         */
        std::string getValue(const std::string& key) const;

        /**
         * @brief 获取指定键名的值指针
         * @param key 头部键名
         * @return 值的指针，不存在时返回 nullptr
         */
        const std::string* getValuePtr(const std::string& key) const;

        /**
         * @brief 移除指定键名的头部字段
         * @param key 头部键名
         * @return 成功返回 kNoError，不存在返回 kHeaderPairNotExist
         */
        HttpErrorCode removeHeaderPair(const std::string& key);

        /**
         * @brief 仅在键名不存在时添加头部字段
         * @param key 头部键名
         * @param value 头部值
         * @return 成功返回 kNoError，已存在返回 kHeaderPairExist
         */
        HttpErrorCode addHeaderPairIfNotExist(const std::string& key, const std::string& value);

        /**
         * @brief 添加头部字段（若键名已存在则覆盖）
         * @param key 头部键名
         * @param value 头部值
         * @return 成功返回 kNoError
         */
        HttpErrorCode addHeaderPair(const std::string& key, const std::string& value);

        /**
         * @brief 添加已规范化的头部字段（fast-path 专用）
         * @param key 已规范化的键名（调用方保证）
         * @param value 头部值
         * @return 成功返回 kNoError
         * @note 仅在解析器内部使用，调用方需确保键名已按当前模式规范化
         */
        HttpErrorCode addNormalizedHeaderPair(std::string key, std::string value);

        /**
         * @brief 估算序列化后的字节大小
         * @return 预估字节数
         */
        size_t estimatedSerializedSize() const;

        /**
         * @brief 将头部字段追加到输出字符串
         * @param out 输出字符串，以 "Key: Value\r\n" 格式追加
         */
        void appendTo(std::string& out) const;

        /**
         * @brief 将头部字段序列化为字符串
         * @return 包含所有键值对的字符串
         */
        std::string toString() const;

        void clear(); ///< 清空所有头部字段

        /**
         * @brief 获取当前存储模式
         * @return 模式枚举值
         */
        Mode mode() const { return m_mode; }

        HeaderPair& operator=(const HeaderPair& other); ///< 拷贝赋值
        HeaderPair& operator=(HeaderPair&& other);      ///< 移动赋值

        /**
         * @brief 设置常见头部字段（fast-path）
         * @param idx 常见头部索引
         * @param value 头部值
         */
        void setCommonHeader(CommonHeaderIndex idx, std::string value);

        /**
         * @brief 获取常见头部字段值（fast-path）
         * @param idx 常见头部索引
         * @return 头部值的 string_view
         */
        std::string_view getCommonHeader(CommonHeaderIndex idx) const;

        /**
         * @brief 检查常见头部字段是否存在（fast-path）
         * @param idx 常见头部索引
         * @return 存在返回 true
         */
        bool hasCommonHeader(CommonHeaderIndex idx) const;

        /**
         * @brief 遍历所有头部字段
         * @param callback 回调函数，参数为 (key, value)
         */
        void forEachHeader(std::function<void(std::string_view, std::string_view)> callback) const;

    private:
        Mode m_mode;                               ///< 存储模式

        std::array<std::string, 15> m_commonHeaders;   ///< Fast-path 存储（仅 ServerSide 使用）
        std::bitset<15> m_commonHeaderPresent;         ///< Fast-path 存在标记

        std::map<std::string, std::string> m_headerPairs; ///< Slow-path 存储
    };

    /**
     * @brief HTTP 请求头
     * @details 封装请求行（方法、URI、版本）与头部字段，支持增量解析。
     */
    class HttpRequestHeader
    {
    public:
        HttpRequestHeader() = default;

        friend class HttpRequest;

        /**
         * @brief 获取请求方法的可变引用
         * @return HTTP 方法枚举引用
         */
        HttpMethod& method();

        /**
         * @brief 获取 URI 的可变引用
         * @return URI 字符串引用
         */
        std::string& uri();

        /**
         * @brief 获取 HTTP 版本的可变引用
         * @return HTTP 版本枚举引用
         */
        HttpVersion& version();

        /**
         * @brief 获取 URI 查询参数映射的可变引用
         * @return 查询参数 map 引用
         */
        std::map<std::string,std::string>& args();

        /**
         * @brief 获取头部键值对的可变引用
         * @return HeaderPair 引用
         */
        HeaderPair& headerPairs();

        /**
         * @brief 将请求头序列化为字符串
         * @return 格式如 "GET /path HTTP/1.1\r\nHost: example.com\r\n\r\n"
         */
        std::string toString() const;

        /**
         * @brief 判断是否为 Keep-Alive 连接
         * @return Keep-Alive 返回 true
         */
        bool isKeepAlive() const;

        /**
         * @brief 判断是否使用 Chunked 传输编码
         * @return 使用 Chunked 返回 true
         */
        bool isChunked() const;

        /**
         * @brief 判断是否为 Connection: close
         * @return 连接即将关闭返回 true
         */
        bool isConnectionClose() const;

        /**
         * @brief 判断请求头是否解析完成
         * @return 解析完成返回 true
         */
        bool isHeaderComplete() const { return m_parseState == RequestParseState::Done; }

        /**
         * @brief 从字符串增量解析请求头
         * @param str 待解析的字符串
         * @return pair.first 为错误码（kNoError/kBadRequest/kVersionNotSupport），
         *         pair.second 为消耗的字节数（>0 完成，0 不完整，-1 错误）
         */
        std::pair<HttpErrorCode, ssize_t> fromString(std::string_view str);

        /**
         * @brief 从 iovec 数组增量解析请求头
         * @param iovecs 离散缓冲区数组
         * @return 同 fromString
         */
        std::pair<HttpErrorCode, ssize_t> fromIOVec(const std::vector<iovec>& iovecs);

        /**
         * @brief 从另一个请求头拷贝内容
         * @param header 源请求头
         */
        void copyFrom(const HttpRequestHeader& header);

        void reset(); ///< 重置所有解析状态与数据

    private:
        /**
         * @brief 解析单个字符
         * @param c 输入字符
         * @return kNoError 继续解析，其他值表示错误或完成
         */
        HttpErrorCode parseChar(char c);
        void commitParsedHeaderPair(); ///< 提交当前解析中的头部键值对
        void parseArgs(std::string uri); ///< 解析 URI 中的查询参数
        std::string convertFromUri(std::string_view url, bool convert_plus_to_space); ///< URL 解码
        std::string convertToUri(std::string&& url) const; ///< URL 编码
        bool isHex(char c, int &v); ///< 判断是否为十六进制字符
        size_t toUtf8(int code, char *buff); ///< 将 Unicode 码点转为 UTF-8
        bool fromHexToI(const std::string_view &s, size_t i, size_t cnt, int &val); ///< 从十六进制字符串解析整数
    private:
        HttpMethod m_method = HttpMethod::GET;               ///< 请求方法
        std::string m_uri;                                    ///< 请求 URI
        HttpVersion m_version = HttpVersion::HttpVersion_1_1; ///< HTTP 版本
        std::map<std::string, std::string> m_argList;         ///< URI 查询参数
        HeaderPair m_headerPairs;                             ///< 头部键值对
        RequestParseState m_parseState = RequestParseState::Method; ///< 解析状态
        std::string m_parseMethodStr;                         ///< 解析中的方法字符串
        std::string m_parseUriStr;                            ///< 解析中的 URI 字符串
        std::string m_parseVersionStr;                        ///< 解析中的版本字符串
        std::string m_parseHeaderKey;                         ///< 解析中的头部键名
        std::string m_parseHeaderValue;                       ///< 解析中的头部值
        size_t m_parsedBytes = 0;                             ///< 已解析的字节数
        CommonHeaderIndex m_currentCommonHeaderIdx = CommonHeaderIndex::NotCommon; ///< 当前解析的常见头部索引
    };

    /**
     * @brief HTTP 响应头
     * @details 封装状态行（版本、状态码）与头部字段，支持增量解析。
     */
    class HttpResponseHeader
    {
    public:
        using ptr = std::shared_ptr<HttpResponseHeader>; ///< 共享指针类型别名

        friend class HttpResponse;

        /**
         * @brief 获取 HTTP 版本的可变引用
         * @return HTTP 版本枚举引用
         */
        HttpVersion& version();

        /**
         * @brief 获取状态码的可变引用
         * @return HttpStatusCode 枚举引用
         */
        HttpStatusCode& code();

        /**
         * @brief 获取头部键值对的可变引用
         * @return HeaderPair 引用
         */
        HeaderPair& headerPairs();

        /**
         * @brief 判断是否为 Keep-Alive 连接
         * @return Keep-Alive 返回 true
         */
        bool isKeepAlive() const;

        /**
         * @brief 判断是否使用 Chunked 传输编码
         * @return 使用 Chunked 返回 true
         */
        bool isChunked() const;

        /**
         * @brief 判断是否为 Connection: close
         * @return 连接即将关闭返回 true
         */
        bool isConnectionClose() const;

        /**
         * @brief 判断响应头是否解析完成
         * @return 解析完成返回 true
         */
        bool isHeaderComplete() const { return m_parseState == ResponseParseState::Done; }

        /**
         * @brief 将响应头序列化为字符串
         * @return 格式如 "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n"
         */
        std::string toString() const;

        /**
         * @brief 从字符串增量解析响应头
         * @param str 待解析的字符串
         * @return pair.first 为错误码（kNoError/kBadRequest/kVersionNotSupport/kHttpCodeInvalid），
         *         pair.second 为消耗的字节数（>0 完成，0 不完整，-1 错误）
         */
        std::pair<HttpErrorCode, ssize_t> fromString(std::string_view str);

        /**
         * @brief 从 iovec 数组增量解析响应头
         * @param iovecs 离散缓冲区数组
         * @return 同 fromString
         */
        std::pair<HttpErrorCode, ssize_t> fromIOVec(const std::vector<iovec>& iovecs);

        /**
         * @brief 从另一个响应头拷贝内容
         * @param header 源响应头
         */
        void copyFrom(const HttpResponseHeader& header);

        void reset(); ///< 重置所有解析状态与数据

    private:
        /**
         * @brief 解析单个字符
         * @param c 输入字符
         * @return kNoError 继续解析，其他值表示错误或完成
         */
        HttpErrorCode parseChar(char c);
        void commitParsedHeaderPair(); ///< 提交当前解析中的头部键值对
    private:
        HttpStatusCode m_code = HttpStatusCode::OK_200;       ///< 状态码
        HttpVersion m_version = HttpVersion::HttpVersion_1_1; ///< HTTP 版本
        HeaderPair m_headerPairs;                             ///< 头部键值对
        ResponseParseState m_parseState = ResponseParseState::Version; ///< 解析状态
        std::string m_parseVersionStr;                        ///< 解析中的版本字符串
        std::string m_parseCodeStr;                           ///< 解析中的状态码字符串
        std::string m_parseHeaderKey;                         ///< 解析中的头部键名
        std::string m_parseHeaderValue;                       ///< 解析中的头部值
        size_t m_parsedBytes = 0;                             ///< 已解析的字节数
        CommonHeaderIndex m_currentCommonHeaderIdx = CommonHeaderIndex::NotCommon; ///< 当前解析的常见头部索引
    };

}


#endif
