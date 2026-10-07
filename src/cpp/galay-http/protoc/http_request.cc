#include "http_request.h"
#include "http_chunk.h"
#include "parse_utils.h"
#include <algorithm>
#include <cctype>
#include <string_view>

namespace galay::http
{
    namespace {

    std::string_view trim_ascii(std::string_view value)
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

    bool is_single_header_token(std::string_view value, std::string_view expected)
    {
        value = trim_ascii(value);
        return value.find(',') == std::string_view::npos &&
               detail::equals_ignore_case_ascii(value, expected);
    }

    } // namespace

    HttpRequestHeader &HttpRequest::header()
    {
       return m_header;
    }

    HttpRequest HttpRequest::clone() const
    {
        HttpRequest copy;
        copy.m_body = m_body;
        copy.m_header = m_header.clone();
        copy.m_contentLength = m_contentLength;
        copy.m_bodyParsed = m_bodyParsed;
        copy.m_headerLength = m_headerLength;
        copy.m_chunkParser = m_chunkParser.clone();
        copy.m_routeParams = m_routeParams.clone();
        if (m_routeParamMapCache.has_value()) {
            auto& cache = copy.m_routeParamMapCache.emplace(*m_routeParamMapCache);
            if (cache.size() != m_routeParamMapCache->size()) {
                copy.m_routeParamMapCache.reset();
            }
        }
        copy.m_headerParsed = m_headerParsed;
        return copy;
    }

    std::string HttpRequest::get_body_str()
    {
        return std::move(m_body);
    }

    const std::string& HttpRequest::body_str() const
    {
        return m_body;
    }

    void HttpRequest::set_header(HttpRequestHeader &&header)
    {
        m_header = std::move(header);
    }

    void HttpRequest::set_header(HttpRequestHeader &header)
    {
        m_header.copy_from(header);
    }

    void HttpRequest::set_body_str(std::string &&body)
    {
        m_body = std::move(body);
    }

    std::string HttpRequest::to_string()
    {
        if(!m_header.is_chunked()) {
            m_header.header_pairs().add_header_pair_if_not_exist("Content-Length", std::to_string(m_body.size()));
        }

        std::string header_str = m_header.to_string();

        if(m_header.is_chunked()) {
            return header_str;
        }

        // 预分配结果字符串，避免临时字符串
        std::string result;
        result.reserve(header_str.size() + m_body.size());
        result += header_str;
        result += m_body;
        return result;
    }

    std::pair<HttpErrorCode, ssize_t> HttpRequest::from_io_vec(const std::vector<iovec>& iovecs)
    {
        return from_io_vec(iovecs, 0);
    }

    std::pair<HttpErrorCode, ssize_t> HttpRequest::from_io_vec(const std::vector<iovec>& iovecs,
                                                             size_t max_body_size)
    {
        ssize_t newly_consumed = 0;
        size_t header_bytes = 0;  // 本次调用中需要跳过的header字节数
        bool is_chunked = false;

        // 如果header还没解析完，先解析header
        if (!m_headerParsed) {
            auto [err, header_consumed] = m_header.from_io_vec(iovecs);
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
                detail::get_header_value_ptr_loose(m_header.header_pairs(), "transfer-encoding");
            const auto* content_length =
                detail::get_header_value_ptr_loose(m_header.header_pairs(), "content-length");

            if (transfer_encoding != nullptr) {
                if (content_length != nullptr) {
                    return {kBadRequest, -1};
                }
                if (!is_single_header_token(*transfer_encoding, "chunked")) {
                    return {kBadRequest, -1};
                }
                is_chunked = true;
            }

            if (is_chunked) {
                if (detail::get_header_value_ptr_loose(m_header.header_pairs(), "content-length") != nullptr) {
                    return {kBadRequest, -1};
                }
                // chunked body 继续往下解析
            } else {
                // header解析完成，获取Content-Length
                if (content_length == nullptr || content_length->empty()) {
                    // 没有body，解析完成
                    return {kNoError, newly_consumed};
                }

                auto parsed_length = detail::parse_size_t_strict(*content_length);
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
            if (const auto* te = detail::get_header_value_ptr_loose(m_header.header_pairs(), "transfer-encoding");
                te != nullptr) {
                is_chunked = detail::header_value_contains_token(*te, "chunked");
            }
        }

        if (is_chunked) {
            auto body_iovecs = detail::slice_iovecs(iovecs, header_bytes);
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
                detail::remove_header_pair_loose(m_header.header_pairs(), "transfer-encoding");
                m_header.header_pairs().add_header_pair("content-length", std::to_string(m_body.size()));
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

    bool HttpRequest::is_complete() const
    {
        if (!m_headerParsed) {
            return false;
        }
        if (m_header.is_chunked()) {
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
        m_routeParamMapCache.reset();
    }

    // ==================== 路由参数方法实现 ====================
    void HttpRequest::set_route_params(std::map<std::string, std::string>&& params)
    {
        RouteParams compact;
        for (const auto& [name, value] : params) {
            const bool inserted = compact.emplace(name, value);
            if (!inserted) {
                compact.clear();
                break;
            }
        }
        m_routeParams = std::move(compact);
        m_routeParamMapCache.reset();
    }

    void HttpRequest::set_route_params(RouteParams&& params)
    {
        m_routeParams = std::move(params);
        m_routeParamMapCache.reset();
    }

    const std::map<std::string, std::string>& HttpRequest::route_params() const
    {
        if (!m_routeParamMapCache.has_value()) {
            auto materialized = m_routeParams.to_map();
            auto& cache = m_routeParamMapCache.emplace(std::move(materialized));
            return cache;
        }
        return *m_routeParamMapCache;
    }

    std::string HttpRequest::get_route_param(const std::string& name, const std::string& defaultValue) const
    {
        const std::string* value = m_routeParams.find(name);
        return value != nullptr ? *value : defaultValue;
    }

    bool HttpRequest::has_route_param(const std::string& name) const
    {
        return m_routeParams.contains(name);
    }
}
