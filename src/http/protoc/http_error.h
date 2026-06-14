/**
 * @file http_error.h
 * @brief HTTP 协议错误码与错误类定义
 * @author galay-http
 * @version 1.0.0
 *
 * @details 定义 HTTP 协议层面所有可能的错误码，以及用于携带错误信息的 HttpError 类。
 */

#ifndef GALAY_HTTP_ERROR_H
#define GALAY_HTTP_ERROR_H

#include "http_base.h"
#include "kernel/common/error.h"
#include <string>

namespace galay::http
{

    /**
     * @brief HTTP 错误码枚举
     * @details 涵盖协议解析、网络 I/O、请求处理等各阶段的错误类型
     */
    enum HttpErrorCode
    {
        kNoError = 0,                          ///< 无错误
        kIncomplete,                           ///< 数据不完整，需要更多数据
        kConnectionClose,                      ///< 连接已关闭
        kTcpRecvError,                         ///< TCP接收错误
        kTcpSendError,                         ///< TCP发送失败
        kRequestTimeOut,                       ///< 请求超时
        kContentLengthNotContained,            ///< 缺少Content-Length
        kContentLengthConvertError,            ///< Content-Length转换错误
        kHeaderInComplete,                     ///< HTTP头部不完整
        kBodyInComplete,                       ///< HTTP体不完整
        kHeaderTooLong,                        ///< HTTP头部过长
        kUriTooLong,                           ///< URI过长
        kChunkHasError,                        ///< 分块传输编码错误
        kHttpCodeInvalid,                      ///< HTTP状态码无效
        kHeaderPairExist,                      ///< HTTP头部键值对已存在
        kHeaderPairNotExist,                   ///< HTTP头部键值对不存在
        kBadRequest,                           ///< 错误的请求格式
        kUrlInvalid,                           ///< URL格式无效
        kPortInvalid,                          ///< 端口号无效
        kMethodNotAllow,                       ///< HTTP方法不支持
        kVersionNotSupport,                    ///< HTTP版本不支持
        kRequestEntityTooLarge,                ///< 请求体过大
        kUriEncodeError,                       ///< URI编码错误
        kContentTypeInvalid,                   ///< Content-Type无效
        kInvalidChunkFormat,                   ///< Chunk格式错误
        kInvalidChunkLength,                   ///< Chunk长度错误
        kBodyLengthNotMatch,                   ///< 请求体长度与Content-Length不匹配
        kRecvTimeOut,                          ///< 接收超时
        kSendTimeOut,                          ///< 发送超时
        kNotFound,                             ///< 未找到
        kNotImplemented,                       ///< 未实现
        kUpgradeFailed,                        ///< 升级失败
        kUnknownError,                         ///< 未知错误
        kHeaderTooLarge,                       ///< HTTP头部过大
        kRecvError,                            ///< 接收错误
        kSendError,                            ///< 发送错误
        kCloseError,                           ///< 关闭错误
        kInternalError,                        ///< 内部错误
        kTcpConnectError,                      ///< TCP连接错误
        kChunkSizeConvertError,                ///< Chunk大小转换错误
    };

    /**
     * @brief HTTP 错误类
     * @details 封装错误码与附加信息，可转换为对应的 HTTP 状态码
     */
    class HttpError
    {
    public:
        /**
         * @brief 构造 HttpError
         * @param code 错误码
         * @param extra_msg 附加错误描述信息
         */
        HttpError(HttpErrorCode code, const std::string& extra_msg = "");

        /**
         * @brief 从底层 I/O 错误构造 HttpError
         * @param io_error galay-kernel 返回的 I/O 错误
         * @details 用于把底层 awaitable/state-machine 的内部错误转换成 HTTP 层错误，
         *          避免 expected<bool, HttpError> 无法承载 IOError 时落入终止进程路径。
         */
        explicit HttpError(const galay::kernel::IOError& io_error);

        /**
         * @brief 获取错误码
         * @return 错误码枚举值
         */
        HttpErrorCode code() const;

        /**
         * @brief 获取错误描述信息
         * @return 错误消息字符串
         */
        std::string message() const;

        /**
         * @brief 将错误码转换为 HTTP 状态码
         * @return 对应的 HttpStatusCode 枚举值
         */
        HttpStatusCode toHttpStatusCode() const;

    private:
        HttpErrorCode m_code;      ///< 错误码
        std::string m_extra_msg;   ///< 附加错误描述
    };


}


#endif
