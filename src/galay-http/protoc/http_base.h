/**
 * @file http_base.h
 * @brief HTTP 协议基础定义
 * @author galay-http
 * @version 1.0.0
 *
 * @details 定义 HTTP 协议的核心枚举类型、常量、MIME 类型映射和全局配置。
 * 包括 HTTP 方法、版本号、状态码枚举，以及协议级别的默认参数。
 */

#ifndef GALAY_HTTP_BASE_H
#define GALAY_HTTP_BASE_H

#include <string>
#include <unordered_map>
#include <atomic>

#define GALAY_VERSION "3.1.0"

namespace galay::http
{

    #define DEFAULT_HTTP_RECV_TIME_MS                       5 * 60 * 1000   ///< 默认 HTTP 接收超时时间（5分钟）
    #define DEFAULT_HTTP_SEND_TIME_MS                       5 * 60 * 1000   ///< 默认 HTTP 发送超时时间（5分钟）
    #define DEFAULT_HTTP_MAX_HEADER_SIZE                    8192            ///< HTTP 头最大长度（8KB）
    #define DEFAULT_HTTP_MAX_BODY_SIZE                      1 * 1024 * 1024 ///< HTTP body 最大长度（1MB）
    #define DEFAULT_HTTP_MAX_URI_LEN                        1024            ///< URI 最大长度（1KB）
    #define DEFAULT_HTTP_MAX_VERSION_SIZE                   32              ///< HTTP 版本字符串最大长度
    #define DEFAULT_HTTP_PEER_STEP_SIZE                     1024            ///< 对端读取步长
    #define DEFAULT_HTTP_CHUNK_BUFFER_SIZE                  2048            ///< Chunk 编码缓冲区大小

    #define DEFAULT_HTTP_KEEPALIVE_TIME_MS                  (7500 * 1000)   ///< 默认 Keep-Alive 超时时间（毫秒）

    #define SERVER_NAME "galay-http"                                        ///< 服务器名称

    #define GALAY_SERVER SERVER_NAME "/" GALAY_VERSION                      ///< 服务器名称及版本标识

    inline std::atomic_int32_t gHttpMaxBodySize = DEFAULT_HTTP_MAX_BODY_SIZE;       ///< 全局 HTTP body 最大长度（线程安全）
    inline std::atomic_int32_t gHttpMaxUriSize = DEFAULT_HTTP_MAX_URI_LEN;           ///< 全局 URI 最大长度（线程安全）
    inline std::atomic_int32_t gHttpMaxVersionSize = DEFAULT_HTTP_MAX_VERSION_SIZE;  ///< 全局版本字符串最大长度（线程安全）


    /**
     * @brief HTTP 请求方法枚举
     * @details 定义 RFC 7231 及 HTTP/2 规范中标准请求方法
     */
    enum class HttpMethod: int
    {
        GET = 0,        ///< GET 方法，获取资源
        POST = 1,       ///< POST 方法，提交数据
        HEAD = 2,       ///< HEAD 方法，获取资源元信息
        PUT = 3,        ///< PUT 方法，替换资源
        DELETE = 4,     ///< DELETE 方法，删除资源
        TRACE = 5,      ///< TRACE 方法，回显请求
        OPTIONS = 6,    ///< OPTIONS 方法，获取支持的方法
        CONNECT = 7,    ///< CONNECT 方法，建立隧道连接
        PATCH = 8,      ///< PATCH 方法，部分更新资源
        PRI = 9,        ///< PRI 方法，HTTP/2 连接前言
        UNKNOWN = 10,   ///< 未知方法
    };

    /**
     * @brief HTTP 协议版本枚举
     * @details 定义 HTTP 协议版本号
     */
    enum class HttpVersion: int
    {
        HttpVersion_1_0,       ///< HTTP/1.0
        HttpVersion_1_1,       ///< HTTP/1.1
        HttpVersion_2_0,       ///< HTTP/2.0
        HttpVersion_3_0,       ///< HTTP/3.0
        HttpVersion_Unknown,   ///< 未知版本
    };

    /**
     * @brief HTTP 状态码枚举
     * @details 定义 RFC 7231 及后续 RFC 中标准状态码，
     *          涵盖 1xx 信息响应、2xx 成功响应、3xx 重定向、4xx 客户端错误、5xx 服务端错误
     */
    enum class HttpStatusCode: int
    {
        // 信息响应（1xx）
        Continue_100 = 100,                      ///< 继续，客户端应继续发送请求
        SwitchingProtocol_101 = 101,             ///< 切换协议
        Processing_102 = 102,                    ///< 处理中（WebDAV）
        EarlyHints_103 = 103,                    ///< 早期提示

        // 成功响应（2xx）
        OK_200 = 200,                            ///< 请求成功
        Created_201 = 201,                       ///< 资源已创建
        Accepted_202 = 202,                      ///< 请求已接受
        NonAuthoritativeInformation_203 = 203,   ///< 非权威信息
        NoContent_204 = 204,                     ///< 无内容
        ResetContent_205 = 205,                  ///< 重置内容
        PartialContent_206 = 206,                ///< 部分内容
        MultiStatus_207 = 207,                   ///< 多状态（WebDAV）
        AlreadyReported_208 = 208,               ///< 已报告（WebDAV）
        IMUsed_226 = 226,                        ///< IM 已使用

        // 重定向消息（3xx）
        MultipleChoices_300 = 300,               ///< 多种选择
        MovedPermanently_301 = 301,              ///< 永久重定向
        Found_302 = 302,                         ///< 临时重定向
        SeeOther_303 = 303,                      ///< 查看其他
        NotModified_304 = 304,                   ///< 未修改
        UseProxy_305 = 305,                      ///< 使用代理
        Unused_306 = 306,                        ///< 未使用
        TemporaryRedirect_307 = 307,             ///< 临时重定向
        PermanentRedirect_308 = 308,             ///< 永久重定向

        // 客户端错误响应（4xx）
        BadRequest_400 = 400,                              ///< 请求格式错误
        Unauthorized_401 = 401,                            ///< 未授权
        PaymentRequired_402 = 402,                         ///< 需要付费
        Forbidden_403 = 403,                               ///< 禁止访问
        NotFound_404 = 404,                                ///< 资源未找到
        MethodNotAllowed_405 = 405,                        ///< 方法不允许
        NotAcceptable_406 = 406,                           ///< 不可接受
        ProxyAuthenticationRequired_407 = 407,             ///< 需要代理认证
        RequestTimeout_408 = 408,                          ///< 请求超时
        Conflict_409 = 409,                                ///< 冲突
        Gone_410 = 410,                                    ///< 资源已删除
        LengthRequired_411 = 411,                          ///< 需要 Content-Length
        PreconditionFailed_412 = 412,                      ///< 前置条件失败
        PayloadTooLarge_413 = 413,                         ///< 请求体过大
        UriTooLong_414 = 414,                              ///< URI 过长
        UnsupportedMediaType_415 = 415,                    ///< 不支持的媒体类型
        RangeNotSatisfiable_416 = 416,                     ///< 范围不满足
        ExpectationFailed_417 = 417,                       ///< 期望失败
        ImATeapot_418 = 418,                               ///< 我是茶壶（RFC 2324）
        MisdirectedRequest_421 = 421,                      ///< 错误定向的请求
        UnprocessableContent_422 = 422,                    ///< 无法处理的实体（WebDAV）
        Locked_423 = 423,                                  ///< 已锁定（WebDAV）
        FailedDependency_424 = 424,                        ///< 依赖失败（WebDAV）
        TooEarly_425 = 425,                                ///< 过早请求
        UpgradeRequired_426 = 426,                         ///< 需要升级协议
        PreconditionRequired_428 = 428,                    ///< 需要前置条件
        TooManyRequests_429 = 429,                         ///< 请求过多
        RequestHeaderFieldsTooLarge_431 = 431,             ///< 请求头字段过大
        UnavailableForLegalReasons_451 = 451,              ///< 因法律原因不可用

        // 服务端错误响应（5xx）
        InternalServerError_500 = 500,                     ///< 服务器内部错误
        NotImplemented_501 = 501,                           ///< 未实现
        BadGateway_502 = 502,                               ///< 网关错误
        ServiceUnavailable_503 = 503,                       ///< 服务不可用
        GatewayTimeout_504 = 504,                           ///< 网关超时
        HttpVersionNotSupported_505 = 505,                  ///< HTTP 版本不支持
        VariantAlsoNegotiates_506 = 506,                    ///< 变体也在协商
        InsufficientStorage_507 = 507,                      ///< 存储空间不足（WebDAV）
        LoopDetected_508 = 508,                             ///< 检测到循环（WebDAV）
        NotExtended_510 = 510,                              ///< 未扩展
        NetworkAuthenticationRequired_511 = 511,            ///< 需要网络认证
    };

    /**
     * @brief 将 HTTP 版本枚举转换为字符串
     * @param version HTTP 版本枚举值
     * @return 版本字符串，如 "HTTP/1.1"
     */
    extern std::string httpVersionToString(HttpVersion version);

    /**
     * @brief 将字符串解析为 HTTP 版本枚举
     * @param str 版本字符串
     * @return 对应的 HTTP 版本枚举值
     */
    extern HttpVersion stringToHttpVersion(std::string_view str);

    /**
     * @brief 将 HTTP 方法枚举转换为字符串
     * @param method HTTP 方法枚举值
     * @return 方法字符串，如 "GET"
     */
    extern std::string httpMethodToString(HttpMethod method);

    /**
     * @brief 将字符串解析为 HTTP 方法枚举
     * @param str 方法字符串
     * @return 对应的 HTTP 方法枚举值
     */
    extern HttpMethod stringToHttpMethod(std::string_view str);

    /**
     * @brief 将 HTTP 状态码枚举转换为状态描述字符串
     * @param code HTTP 状态码枚举值
     * @return 状态描述字符串，如 "OK"、"Not Found"
     */
    extern std::string httpStatusCodeToString(HttpStatusCode code);

    /**
     * @brief MIME 类型映射工具类
     * @details 根据文件扩展名查找对应的 MIME 类型，
     *          内部维护一个文件扩展名到 MIME 类型的映射表。
     */
    class MimeType
    {
    public:
        /**
         * @brief 将文件扩展名转换为 MIME 类型
         * @param type 文件扩展名（不含点号），如 "html"、"json"
         * @return 对应的 MIME 类型字符串，未找到时返回 "application/octet-stream"
         */
        static std::string convertToMimeType(const std::string& type);
    private:
        static std::unordered_map<std::string, std::string> mimeTypeMap; ///< 文件扩展名到 MIME 类型的映射表
    };

    // HTTP 方法常量（避免使用宏以防止与其他库冲突）
    constexpr HttpMethod HTTP_GET = HttpMethod::GET;               ///< GET 方法常量
    constexpr HttpMethod HTTP_POST = HttpMethod::POST;             ///< POST 方法常量
    constexpr HttpMethod HTTP_HEAD = HttpMethod::HEAD;             ///< HEAD 方法常量
    constexpr HttpMethod HTTP_PUT = HttpMethod::PUT;               ///< PUT 方法常量
    constexpr HttpMethod HTTP_DELETE = HttpMethod::DELETE;         ///< DELETE 方法常量
    constexpr HttpMethod HTTP_TRACE = HttpMethod::TRACE;           ///< TRACE 方法常量
    constexpr HttpMethod HTTP_OPTIONS = HttpMethod::OPTIONS;       ///< OPTIONS 方法常量
    constexpr HttpMethod HTTP_CONNECT = HttpMethod::CONNECT;       ///< CONNECT 方法常量
    constexpr HttpMethod HTTP_PATCH = HttpMethod::PATCH;           ///< PATCH 方法常量
    constexpr HttpMethod HTTP_PRI = HttpMethod::PRI;               ///< PRI 方法常量
    constexpr HttpMethod HTTP_UNKNOWN = HttpMethod::UNKNOWN;       ///< UNKNOWN 方法常量

    constexpr HttpVersion HTTP_VERSION_1_0 = HttpVersion::HttpVersion_1_0;  ///< HTTP/1.0 版本常量
    constexpr HttpVersion HTTP_VERSION_1_1 = HttpVersion::HttpVersion_1_1;  ///< HTTP/1.1 版本常量
    constexpr HttpVersion HTTP_VERSION_2_0 = HttpVersion::HttpVersion_2_0;  ///< HTTP/2.0 版本常量
}

#endif
