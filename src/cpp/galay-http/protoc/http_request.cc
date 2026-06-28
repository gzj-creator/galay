#include "http_request.h"
#include "http_chunk.h"
#include "parse_utils.h"
#include <algorithm>
#include <cctype>
#include <string_view>

namespace galay::http
{
    namespace {

    std::string_view trimAscii(std::string_view value)
    {
        size_t begin = 0;
        size_t end = value.size();
        while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) {
            ++begin;
        }
        while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
            --end;
        }
        return value.substr(begin, end - begin);
    }

    bool isSingleHeaderToken(std::string_view value, std::string_view expected)
    {
        value = trimAscii(value);
        return value.find(',') == std::string_view::npos &&
               detail::equalsIgnoreCaseAscii(value, expected);
    }

    } // namespace

    HttpRequestHeader &HttpRequest::header()
    {
       return m_header;
    }

    std::string HttpRequest::getBodyStr()
    {
        return std::move(m_body);
    }

    const std::string& HttpRequest::bodyStr() const
    {
        return m_body;
    }

    void HttpRequest::setHeader(HttpRequestHeader &&header)
    {
        m_header = std::move(header);
    }

    void HttpRequest::setHeader(HttpRequestHeader &header)
    {
        m_header.copyFrom(header);
    }

    void HttpRequest::setBodyStr(std::string &&body)
    {
        m_body = std::move(body);
    }

    std::string HttpRequest::toString()
    {
        if(!m_header.isChunked()) {
            m_header.headerPairs().addHeaderPairIfNotExist("Content-Length", std::to_string(m_body.size()));
        }

        std::string header_str = m_header.toString();

        if(m_header.isChunked()) {
            return header_str;
        }

        // 预分配结果字符串，避免临时字符串
        std::string result;
        result.reserve(header_str.size() + m_body.size());
        result += header_str;
        result += m_body;
        return result;
    }

    std::pair<HttpErrorCode, ssize_t> HttpRequest::fromIOVec(const std::vector<iovec>& iovecs)
    {
        return fromIOVec(iovecs, 0);
    }

    std::pair<HttpErrorCode, ssize_t> HttpRequest::fromIOVec(const std::vector<iovec>& iovecs,
                                                             size_t max_body_size)
    {
        ssize_t newly_consumed = 0;
        size_t header_bytes = 0;  // 本次调用中需要跳过的header字节数
        bool is_chunked = false;

        // 如果header还没解析完，先解析header
        if (!m_headerParsed) {
            auto [err, header_consumed] = m_header.fromIOVec(iovecs);
            if (err != kNoError && err != kIncomplete) {
                return {err, -1};
            }
            if (err == kIncomplete) {
                // header数据不完整，返回已消费的字节数
                return {kNoError, header_consumed};
            }
            // header解析完成 (err == kNoError && header_consumed > 0)
            header_bytes = header_consumed;
            newly_consumed = header_consumed;
            m_headerParsed = true;
            const auto* transfer_encoding =
                detail::getHeaderValuePtrLoose(m_header.headerPairs(), "transfer-encoding");
            const auto* content_length =
                detail::getHeaderValuePtrLoose(m_header.headerPairs(), "content-length");

            if (transfer_encoding != nullptr) {
                if (content_length != nullptr) {
                    return {kBadRequest, -1};
                }
                if (!isSingleHeaderToken(*transfer_encoding, "chunked")) {
                    return {kBadRequest, -1};
                }
                is_chunked = true;
            }

            if (is_chunked) {
                if (detail::getHeaderValuePtrLoose(m_header.headerPairs(), "content-length") != nullptr) {
                    return {kBadRequest, -1};
                }
                // chunked body 继续往下解析
            } else {
                // header解析完成，获取Content-Length
                if (content_length == nullptr || content_length->empty()) {
                    // 没有body，解析完成
                    return {kNoError, newly_consumed};
                }

                auto parsed_length = detail::parseSizeTStrict(*content_length);
                if (!parsed_length.has_value()) {
                    return {kBadRequest, -1};
                }
                m_contentLength = parsed_length.value();
                if (max_body_size != 0 && m_contentLength > max_body_size) {
                    return {kRequestEntityTooLarge, -1};
                }

                if (m_contentLength == 0) {
                    return {kNoError, newly_consumed};
                }
                constexpr size_t kInitialBodyReserveLimit = 64 * 1024;
                m_body.reserve(std::min(m_contentLength, kInitialBodyReserveLimit));
            }
        } else {
            if (const auto* te = detail::getHeaderValuePtrLoose(m_header.headerPairs(), "transfer-encoding");
                te != nullptr) {
                is_chunked = detail::headerValueContainsToken(*te, "chunked");
            }
        }

        if (is_chunked) {
            auto body_iovecs = detail::sliceIovecs(iovecs, header_bytes);
            if (body_iovecs.empty()) {
                return {kNoError, newly_consumed};
            }

            auto chunk_result = m_chunkParser.parse(body_iovecs, m_body, max_body_size);
            if (!chunk_result) {
                if (chunk_result.error().code() == kIncomplete) {
                    return {kNoError, newly_consumed};
                }
                return {chunk_result.error().code(), -1};
            }

            newly_consumed += chunk_result.value().second;

            if (chunk_result.value().first) {
                detail::removeHeaderPairLoose(m_header.headerPairs(), "transfer-encoding");
                m_header.headerPairs().addHeaderPair("content-length", std::to_string(m_body.size()));
                m_contentLength = m_body.size();
                m_bodyParsed = m_contentLength;
            }

            return {kNoError, newly_consumed};
        }

        // 如果没有body需要解析，直接返回
        if (m_contentLength == 0) {
            return {kNoError, 0};
        }

        size_t iov_idx = 0;
        size_t byte_idx = 0;

        // 跳过header部分（仅当本次调用解析了header时）
        while (header_bytes > 0 && iov_idx < iovecs.size()) {
            size_t available = iovecs[iov_idx].iov_len - byte_idx;
            if (available <= header_bytes) {
                header_bytes -= available;
                ++iov_idx;
                byte_idx = 0;
            } else {
                byte_idx += header_bytes;
                header_bytes = 0;
            }
        }

        // 读取body
        size_t body_needed = m_contentLength - m_bodyParsed;
        size_t body_read = 0;
        while (body_read < body_needed && iov_idx < iovecs.size()) {
            size_t available = iovecs[iov_idx].iov_len - byte_idx;
            size_t to_read = std::min(available, body_needed - body_read);
            m_body.append(static_cast<const char*>(iovecs[iov_idx].iov_base) + byte_idx, to_read);
            body_read += to_read;
            if (to_read == available) {
                ++iov_idx;
                byte_idx = 0;
            } else {
                byte_idx += to_read;
            }
        }

        m_bodyParsed += body_read;
        newly_consumed += body_read;

        return {kNoError, newly_consumed};
    }

    bool HttpRequest::isComplete() const
    {
        if (!m_headerParsed) {
            return false;
        }
        if (m_header.isChunked()) {
            return false; // chunked需要单独处理
        }
        return m_bodyParsed >= m_contentLength;
    }

    void HttpRequest::reset()
    {
        m_header.reset();
        m_body.clear();
        m_contentLength = 0;
        m_bodyParsed = 0;
        m_headerParsed = false;
        m_chunkParser.reset();
        m_routeParams.clear();
    }

    // ==================== 路由参数方法实现 ====================
    void HttpRequest::setRouteParams(std::map<std::string, std::string>&& params)
    {
        m_routeParams = std::move(params);
    }

    const std::map<std::string, std::string>& HttpRequest::routeParams() const
    {
        return m_routeParams;
    }

    std::string HttpRequest::getRouteParam(const std::string& name, const std::string& defaultValue) const
    {
        auto it = m_routeParams.find(name);
        return it != m_routeParams.end() ? it->second : defaultValue;
    }

    bool HttpRequest::hasRouteParam(const std::string& name) const
    {
        return m_routeParams.find(name) != m_routeParams.end();
    }
}
