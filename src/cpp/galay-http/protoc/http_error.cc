#include "http_error.h"

namespace galay::http
{
    const char* g_http_error_messages[] = {
        "No error",                           // kNoError = 0
        "Incomplete data",                    // kIncomplete = 1
        "Connection closed",                  // kConnectionClose = 2
        "Tcp recv error",                     // kTcpRecvError = 3
        "Tcp send error",                     // kTcpSendError = 4
        "Request timeout",                    // kRequestTimeOut = 5
        "Not contains Content-Length",        // kContentLengthNotContained = 6
        "Content-Length convert error",       // kContentLengthConvertError = 7
        "HTTP header incomplete",             // kHeaderInComplete = 8
        "HTTP body incomplete",               // kBodyInComplete = 9
        "HTTP header too long",               // kHeaderTooLong = 10
        "URI too long",                       // kUriTooLong = 11
        "Chunked transfer encoding error",    // kChunkHasError = 12
        "Invalid HTTP status code",           // kHttpCodeInvalid = 13
        "HTTP header key-value pair already exists",  // kHeaderPairExist = 14
        "HTTP header key-value pair does not exist",  // kHeaderPairNotExist = 15
        "Bad request format",                 // kBadRequest = 16
        "Invalid URL format",                 // kUrlInvalid = 17
        "Invalid port number",                // kPortInvalid = 18
        "HTTP method not allowed",            // kMethodNotAllow = 19
        "HTTP version not supported",         // kVersionNotSupport = 20
        "Request entity too large",           // kRequestEntityTooLarge = 21
        "URI encoding error",                 // kUriEncodeError = 22
        "Invalid Content-Type",               // kContentTypeInvalid = 23
        "Invalid chunk format",               // kInvalidChunkFormat = 24
        "Invalid chunk length",               // kInvalidChunkLength = 25
        "Body length not match Content-Length",// kBodyLengthNotMatch = 26
        "Recv time out",                       // kRecvTimeOut = 27
        "Send timeout",                        // kSendTimeOut = 28
        "Not found",                           // kNotFound = 29
        "Not implemented",                     // kNotImplemented = 30
        "Upgrade failed",                      // kUpgradeFailed = 31
        "Unknown error",                       // kUnknownError = 32
        "HTTP header too large",               // kHeaderTooLarge = 33
        "Recv error",                          // kRecvError = 34
        "Send error",                          // kSendError = 35
        "Close error",                         // kCloseError = 36
        "Internal error",                      // kInternalError = 37
        "TCP connect error",                   // kTcpConnectError = 38
        "Chunk size convert error",            // kChunkSizeConvertError = 39
    };


    HttpError::HttpError(HttpErrorCode code, const std::string& extra_msg)
        : m_extra_msg(extra_msg)
        , m_code(code)
    {
    }

    HttpError::HttpError(const galay::kernel::IOError& io_error)
        : m_extra_msg(io_error.message())
        , m_code(kInternalError)
    {
        const uint64_t code = io_error.code();
        if (galay::kernel::IOError::contains(code, galay::kernel::kDisconnectError)) {
            m_code = kConnectionClose;
        } else if (galay::kernel::IOError::contains(code, galay::kernel::kTimeout)) {
            m_code = kRecvTimeOut;
        } else if (galay::kernel::IOError::contains(code, galay::kernel::kRecvFailed) ||
                   galay::kernel::IOError::contains(code, galay::kernel::kReadFailed)) {
            m_code = kRecvError;
        } else if (galay::kernel::IOError::contains(code, galay::kernel::kSendFailed) ||
                   galay::kernel::IOError::contains(code, galay::kernel::kWriteFailed)) {
            m_code = kSendError;
        } else if (galay::kernel::IOError::contains(code, galay::kernel::kConnectFailed)) {
            m_code = kTcpConnectError;
        }
    }

#ifdef GALAY_SSL_FEATURE_ENABLED
    HttpError::HttpError(const galay::ssl::SslError& ssl_error)
        : m_extra_msg(ssl_error.message())
        , m_code(kInternalError)
    {
        using galay::ssl::SslErrorCode;

        switch (ssl_error.code()) {
        case SslErrorCode::kTimeout:
        case SslErrorCode::kHandshakeTimeout:
            m_code = kRequestTimeOut;
            return;
        case SslErrorCode::kPeerClosed:
            m_code = kConnectionClose;
            return;
        case SslErrorCode::kReadFailed:
            m_code = kRecvError;
            return;
        case SslErrorCode::kWriteFailed:
            m_code = kSendError;
            return;
        case SslErrorCode::kVerificationFailed:
        case SslErrorCode::kHandshakeFailed:
        case SslErrorCode::kSNISetFailed:
            m_code = kTcpConnectError;
            return;
        default:
            m_code = kInternalError;
            return;
        }
    }
#endif

    HttpErrorCode HttpError::code() const
    {
        return m_code;
    }

    std::string HttpError::message() const
    {
        constexpr size_t message_count = sizeof(g_http_error_messages) / sizeof(g_http_error_messages[0]);
        if(static_cast<uint32_t>(m_code) >= message_count) {
            return "Unknown Http error";
        }
        if (m_extra_msg.empty()) {
            return std::string(g_http_error_messages[m_code]);
        }
        return std::string(g_http_error_messages[m_code]) + ": " + m_extra_msg;
    }

    HttpStatusCode HttpError::toHttpStatusCode() const
    {
        switch (m_code)
        {
            case kNoError:
                return HttpStatusCode::OK_200;
                
            case kConnectionClose:
                return HttpStatusCode::InternalServerError_500;
                
            case kTcpRecvError:
            case kTcpSendError:
                return HttpStatusCode::InternalServerError_500;
                
            case kRequestTimeOut:
            case kRecvTimeOut:
            case kSendTimeOut:
                return HttpStatusCode::RequestTimeout_408;
                
            case kContentLengthNotContained:
                return HttpStatusCode::LengthRequired_411;
                
            case kContentLengthConvertError:
            case kBodyLengthNotMatch:
                return HttpStatusCode::BadRequest_400;
                
            case kHeaderInComplete:
            case kBadRequest:
            case kUrlInvalid:
            case kUriEncodeError:
            case kInvalidChunkFormat:
            case kInvalidChunkLength:
            case kChunkHasError:
                return HttpStatusCode::BadRequest_400;
                
            case kBodyInComplete:
                return HttpStatusCode::BadRequest_400;
                
            case kHeaderTooLong:
            case kHeaderTooLarge:
                return HttpStatusCode::RequestHeaderFieldsTooLarge_431;
                
            case kUriTooLong:
                return HttpStatusCode::UriTooLong_414;
                
            case kHttpCodeInvalid:
                return HttpStatusCode::BadRequest_400;
                
            case kHeaderPairExist:
            case kHeaderPairNotExist:
                return HttpStatusCode::BadRequest_400;
                
            case kPortInvalid:
                return HttpStatusCode::BadRequest_400;
                
            case kMethodNotAllow:
                return HttpStatusCode::MethodNotAllowed_405;
                
            case kVersionNotSupport:
                return HttpStatusCode::HttpVersionNotSupported_505;
                
            case kRequestEntityTooLarge:
                return HttpStatusCode::PayloadTooLarge_413;
                
            case kContentTypeInvalid:
                return HttpStatusCode::UnsupportedMediaType_415;
            case kNotFound:
                return HttpStatusCode::NotFound_404;
            case kUnknownError:
                return HttpStatusCode::InternalServerError_500;
            case kNotImplemented:
                return HttpStatusCode::NotImplemented_501;
            case kUpgradeFailed:
                return HttpStatusCode::UpgradeRequired_426;
            default:
                return HttpStatusCode::InternalServerError_500;
        }
    }
}
