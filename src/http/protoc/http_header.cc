#include "http_header.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <string_view>

namespace galay::http
{
    namespace {

    inline char toLowerAsciiChar(char ch)
    {
        if (ch >= 'A' && ch <= 'Z') {
            return static_cast<char>(ch + ('a' - 'A'));
        }
        return ch;
    }

    bool parseHttpStatusCode(std::string_view text, HttpStatusCode& code)
    {
        if (text.empty()) {
            return false;
        }

        int parsed = 0;
        const char* begin = text.data();
        const char* end = begin + text.size();
        auto [ptr, ec] = std::from_chars(begin, end, parsed);
        if (ec != std::errc{} || ptr != end) {
            return false;
        }

        code = static_cast<HttpStatusCode>(parsed);
        return true;
    }

    // 快速匹配常见 header（假设 key 已是小写）
    CommonHeaderIndex matchCommonHeader(const std::string& key) {
        const size_t len = key.size();
        if (len < 4 || len > 19) return CommonHeaderIndex::NotCommon;

        switch (key[0]) {
        case 'h':
            if (len == 4 && key == "host")
                return CommonHeaderIndex::Host;
            break;

        case 'c':
            if (len == 14 && key == "content-length")
                return CommonHeaderIndex::ContentLength;
            if (len == 12 && key == "content-type")
                return CommonHeaderIndex::ContentType;
            if (len == 10 && key == "connection")
                return CommonHeaderIndex::Connection;
            if (len == 13 && key == "cache-control")
                return CommonHeaderIndex::CacheControl;
            if (len == 6 && key == "cookie")
                return CommonHeaderIndex::Cookie;
            break;

        case 'u':
            if (len == 10 && key == "user-agent")
                return CommonHeaderIndex::UserAgent;
            break;

        case 'a':
            if (len == 6 && key == "accept")
                return CommonHeaderIndex::Accept;
            if (len == 15) {
                if (key == "accept-encoding")
                    return CommonHeaderIndex::AcceptEncoding;
                if (key == "accept-language")
                    return CommonHeaderIndex::AcceptLanguage;
            }
            if (len == 13 && key == "authorization")
                return CommonHeaderIndex::Authorization;
            break;

        case 'i':
            if (len == 17 && key == "if-modified-since")
                return CommonHeaderIndex::IfModifiedSince;
            if (len == 13 && key == "if-none-match")
                return CommonHeaderIndex::IfNoneMatch;
            break;

        case 'r':
            if (len == 7 && key == "referer")
                return CommonHeaderIndex::Referer;
            if (len == 5 && key == "range")
                return CommonHeaderIndex::Range;
            break;
        }

        return CommonHeaderIndex::NotCommon;
    }

    // 获取常见 header 的标准名称（小写）
    std::string_view getCommonHeaderName(CommonHeaderIndex idx) {
        static const std::array<std::string_view, 15> names = {
            "host",
            "content-length",
            "content-type",
            "user-agent",
            "accept",
            "accept-encoding",
            "connection",
            "cache-control",
            "cookie",
            "authorization",
            "if-modified-since",
            "if-none-match",
            "referer",
            "accept-language",
            "range"
        };
        return names[static_cast<size_t>(idx)];
    }

    inline char toUpperAsciiChar(char ch)
    {
        if (ch >= 'a' && ch <= 'z') {
            return static_cast<char>(ch - ('a' - 'A'));
        }
        return ch;
    }

    inline bool hasUpperAscii(std::string_view value)
    {
        for (char ch : value) {
            if (ch >= 'A' && ch <= 'Z') {
                return true;
            }
        }
        return false;
    }

    std::string toLowerAscii(std::string value)
    {
        for (char& ch : value) {
            ch = toLowerAsciiChar(ch);
        }
        return value;
    }

    std::string toCanonicalHeaderKey(const std::string& value)
    {
        std::string result = value;
        bool word_start = true;
        for (char& ch : result) {
            if (word_start) {
                ch = toUpperAsciiChar(ch);
            } else {
                ch = toLowerAsciiChar(ch);
            }
            word_start = (ch == '-');
        }
        return result;
    }

    bool equalsIgnoreCaseAscii(std::string_view lhs, std::string_view rhs)
    {
        if (lhs.size() != rhs.size()) {
            return false;
        }

        for (size_t i = 0; i < lhs.size(); ++i) {
            if (toLowerAsciiChar(lhs[i]) != toLowerAsciiChar(rhs[i])) {
                return false;
            }
        }
        return true;
    }

    std::string normalizeKey(HeaderPair::Mode mode, const std::string& key)
    {
        switch (mode) {
        case HeaderPair::Mode::ServerSide:
            if (!hasUpperAscii(key)) {
                return key;
            }
            return toLowerAscii(key);
        case HeaderPair::Mode::ClientSide:   return toCanonicalHeaderKey(key);
        }
        return key;
    }

    std::map<std::string, std::string>::iterator findHeaderPairIter(
        HeaderPair::Mode mode,
        std::map<std::string, std::string>& headers,
        const std::string& key)
    {
        if (mode == HeaderPair::Mode::ServerSide) {
            auto it = headers.find(key);
            if (it != headers.end()) {
                return it;
            }
            if (!hasUpperAscii(key)) {
                return headers.end();
            }

            std::string normalized = toLowerAscii(key);
            return headers.find(normalized);
        }

        // ClientSide mode
        auto it = headers.find(key);
        if (it != headers.end()) {
            return it;
        }

        std::string normalized = toCanonicalHeaderKey(key);
        if (normalized == key) {
            return headers.end();
        }
        return headers.find(normalized);
    }

    std::map<std::string, std::string>::const_iterator findHeaderPairIter(
        HeaderPair::Mode mode,
        const std::map<std::string, std::string>& headers,
        const std::string& key)
    {
        if (mode == HeaderPair::Mode::ServerSide) {
            auto it = headers.find(key);
            if (it != headers.end()) {
                return it;
            }
            if (!hasUpperAscii(key)) {
                return headers.end();
            }

            std::string normalized = toLowerAscii(key);
            return headers.find(normalized);
        }

        // ClientSide mode
        auto it = headers.find(key);
        if (it != headers.end()) {
            return it;
        }

        std::string normalized = toCanonicalHeaderKey(key);
        if (normalized == key) {
            return headers.end();
        }
        return headers.find(normalized);
    }

    inline char normalizeHeaderKeyChar(char ch, HeaderPair::Mode mode)
    {
        if (mode == HeaderPair::Mode::ServerSide) {
            return toLowerAsciiChar(ch);
        }
        return ch;
    }

    inline void reserveIfUnset(std::string& text, size_t capacity_hint)
    {
        if (text.capacity() == 0) {
            text.reserve(capacity_hint);
        }
    }

    bool headerValueContainsToken(std::string_view value, std::string_view token)
    {
        if (value.empty() || token.empty()) {
            return false;
        }

        size_t start = 0;
        while (start < value.size()) {
            size_t end = value.find(',', start);
            if (end == std::string_view::npos) {
                end = value.size();
            }

            size_t left = start;
            while (left < end && std::isspace(static_cast<unsigned char>(value[left]))) {
                ++left;
            }
            size_t right = end;
            while (right > left && std::isspace(static_cast<unsigned char>(value[right - 1]))) {
                --right;
            }

            if (right > left && equalsIgnoreCaseAscii(value.substr(left, right - left), token)) {
                return true;
            }

            start = end + 1;
        }

        return false;
    }

    HttpMethod parseHttpMethodFast(std::string_view method)
    {
        if (method == "GET") {
            return HttpMethod::GET;
        }
        if (method == "POST") {
            return HttpMethod::POST;
        }
        if (method == "HEAD") {
            return HttpMethod::HEAD;
        }
        if (method == "PUT") {
            return HttpMethod::PUT;
        }
        if (method == "DELETE") {
            return HttpMethod::DELETE;
        }
        if (method == "TRACE") {
            return HttpMethod::TRACE;
        }
        if (method == "OPTIONS") {
            return HttpMethod::OPTIONS;
        }
        if (method == "CONNECT") {
            return HttpMethod::CONNECT;
        }
        if (method == "PATCH") {
            return HttpMethod::PATCH;
        }
        if (method == "PRI") {
            return HttpMethod::PRI;
        }
        return HttpMethod::UNKNOWN;
    }

    HttpVersion parseHttpVersionFast(std::string_view version)
    {
        if (version == "HTTP/1.1") {
            return HttpVersion::HttpVersion_1_1;
        }
        if (version == "HTTP/1.0") {
            return HttpVersion::HttpVersion_1_0;
        }
        if (version == "HTTP/2.0") {
            return HttpVersion::HttpVersion_2_0;
        }
        if (version == "HTTP/3.0") {
            return HttpVersion::HttpVersion_3_0;
        }
        return HttpVersion::HttpVersion_Unknown;
    }

    } // namespace

    HeaderPair::HeaderPair(Mode mode)
        : m_mode(mode)
        , m_commonHeaderPresent(0)  // 初始化为全 0
    {
    }

    HeaderPair::HeaderPair(const HeaderPair &other)
        : m_mode(other.m_mode)
        , m_commonHeaders(other.m_commonHeaders)
        , m_commonHeaderPresent(other.m_commonHeaderPresent)
        , m_headerPairs(other.m_headerPairs)
    {
    }

    HeaderPair::HeaderPair(HeaderPair &&other)
        : m_mode(other.m_mode)
        , m_commonHeaders(std::move(other.m_commonHeaders))
        , m_commonHeaderPresent(other.m_commonHeaderPresent)
        , m_headerPairs(std::move(other.m_headerPairs))
    {
    }

    bool HeaderPair::hasKey(const std::string &key) const
    {
        return getValuePtr(key) != nullptr;
    }

    std::string HeaderPair::getValue(const std::string& key) const
    {
        if (const auto* value = getValuePtr(key); value != nullptr) {
            return *value;
        }
        return {};
    }

    const std::string* HeaderPair::getValuePtr(const std::string& key) const
    {
        // ServerSide 模式：先尝试 fast-path
        if (m_mode == Mode::ServerSide) {
            // 需要先转小写再匹配
            std::string normalized;
            if (hasUpperAscii(key)) {
                normalized = toLowerAscii(key);
            } else {
                normalized = key;
            }

            CommonHeaderIndex idx = matchCommonHeader(normalized);
            if (idx != CommonHeaderIndex::NotCommon) {
                if (hasCommonHeader(idx)) {
                    return &m_commonHeaders[static_cast<size_t>(idx)];
                }
                return nullptr;
            }
        }

        // Fallback: 在 map 中查找
        auto it = findHeaderPairIter(m_mode, m_headerPairs, key);
        if (it == m_headerPairs.end()) {
            return nullptr;
        }
        return &it->second;
    }

    HttpErrorCode HeaderPair::removeHeaderPair(const std::string& key)
    {
        if (m_mode == Mode::ServerSide) {
            std::string normalized;
            if (!hasUpperAscii(key)) {
                normalized = key;
            } else {
                normalized = toLowerAscii(key);
            }

            CommonHeaderIndex idx = matchCommonHeader(normalized);
            if (idx != CommonHeaderIndex::NotCommon) {
                const size_t i = static_cast<size_t>(idx);
                if (m_commonHeaderPresent.test(i)) {
                    m_commonHeaders[i].clear();
                    m_commonHeaderPresent.reset(i);
                    return kNoError;
                }
            }
        }

        auto it = findHeaderPairIter(m_mode, m_headerPairs, key);
        if (it == m_headerPairs.end()) {
            return kHeaderPairNotExist;
        }
        m_headerPairs.erase(it);
        return kNoError;
    }

    HttpErrorCode HeaderPair::addHeaderPairIfNotExist(const std::string& key, const std::string& value)
    {
        if (m_mode == Mode::ServerSide) {
            std::string normalized;
            if (!hasUpperAscii(key)) {
                normalized = key;
            } else {
                normalized = toLowerAscii(key);
            }

            CommonHeaderIndex idx = matchCommonHeader(normalized);
            if (idx != CommonHeaderIndex::NotCommon) {
                if (hasCommonHeader(idx)) {
                    return kHeaderPairExist;
                }
                setCommonHeader(idx, value);
                return kNoError;
            }

            auto [it, inserted] = m_headerPairs.try_emplace(std::move(normalized), value);
            (void)it;
            return inserted ? kNoError : kHeaderPairExist;
        }

        // ClientSide mode
        std::string normalized = toCanonicalHeaderKey(key);
        auto [it, inserted] = m_headerPairs.try_emplace(std::move(normalized), value);
        (void)it;
        return inserted ? kNoError : kHeaderPairExist;
    }

    HttpErrorCode HeaderPair::addHeaderPair(const std::string& key, const std::string& value)
    {
        if (m_mode == Mode::ServerSide) {
            std::string normalized;
            if (!hasUpperAscii(key)) {
                normalized = key;
            } else {
                normalized = toLowerAscii(key);
            }

            // 尝试使用 fast-path
            CommonHeaderIndex idx = matchCommonHeader(normalized);
            if (idx != CommonHeaderIndex::NotCommon) {
                setCommonHeader(idx, value);
                return kNoError;
            }

            // Fallback: 存入 map
            m_headerPairs.insert_or_assign(std::move(normalized), value);
            return kNoError;
        }

        // ClientSide mode
        std::string normalized = toCanonicalHeaderKey(key);
        m_headerPairs.insert_or_assign(std::move(normalized), value);
        return kNoError;
    }

    HttpErrorCode HeaderPair::addNormalizedHeaderPair(std::string key, std::string value)
    {
        // ServerSide 模式：尝试使用 fast-path
        if (m_mode == Mode::ServerSide) {
            CommonHeaderIndex idx = matchCommonHeader(key);
            if (idx != CommonHeaderIndex::NotCommon) {
                // 直接替换（不追加），与 insert_or_assign 行为一致
                size_t i = static_cast<size_t>(idx);
                m_commonHeaders[i] = std::move(value);
                m_commonHeaderPresent.set(i);
                return kNoError;
            }
        }

        // Fallback: 存入 map
        m_headerPairs.insert_or_assign(std::move(key), std::move(value));
        return kNoError;
    }

    size_t HeaderPair::estimatedSerializedSize() const
    {
        size_t estimated_size = 0;

        // 计算 common headers 的大小
        for (size_t i = 0; i < m_commonHeaders.size(); ++i) {
            if (m_commonHeaderPresent.test(i)) {
                estimated_size += getCommonHeaderName(static_cast<CommonHeaderIndex>(i)).size();
                estimated_size += m_commonHeaders[i].size();
                estimated_size += 4; // ": " + "\r\n"
            }
        }

        // 计算 map 中其他 headers 的大小
        for (const auto& [k, v] : m_headerPairs) {
            estimated_size += k.size() + v.size() + 4; // "key: value\r\n"
        }
        return estimated_size;
    }

    void HeaderPair::appendTo(std::string& out) const
    {
        // 先输出 common headers
        for (size_t i = 0; i < m_commonHeaders.size(); ++i) {
            if (m_commonHeaderPresent.test(i)) {
                out += getCommonHeaderName(static_cast<CommonHeaderIndex>(i));
                out += ": ";
                out += m_commonHeaders[i];
                out += "\r\n";
            }
        }

        // 再输出 map 中的其他 headers
        for (const auto& [k, v] : m_headerPairs) {
            out += k;
            out += ": ";
            out += v;
            out += "\r\n";
        }
    }

    std::string HeaderPair::toString() const
    {
        if (m_headerPairs.empty() && m_commonHeaderPresent.none()) {
            return "";
        }

        std::string result;
        result.reserve(estimatedSerializedSize());
        appendTo(result);
        return result;
    }

    void HeaderPair::clear()
    {
        if (m_mode == Mode::ServerSide) {
            for (size_t i = 0; i < m_commonHeaders.size(); ++i) {
                if (m_commonHeaderPresent.test(i)) {
                    m_commonHeaders[i].clear();
                }
            }
            m_commonHeaderPresent.reset();
        }
        if(!m_headerPairs.empty()) m_headerPairs.clear();
    }

    HeaderPair &HeaderPair::operator=(const HeaderPair &other)
    {
        if (this != &other) {
            m_mode = other.m_mode;
            m_commonHeaders = other.m_commonHeaders;
            m_commonHeaderPresent = other.m_commonHeaderPresent;
            m_headerPairs = other.m_headerPairs;
        }
        return *this;
    }

    HeaderPair &HeaderPair::operator=(HeaderPair &&other)
    {
        if (this != &other) {
            m_mode = other.m_mode;
            m_commonHeaders = std::move(other.m_commonHeaders);
            m_commonHeaderPresent = other.m_commonHeaderPresent;
            m_headerPairs = std::move(other.m_headerPairs);
        }
        return *this;
    }

    void HeaderPair::setCommonHeader(CommonHeaderIndex idx, std::string value)
    {
        size_t i = static_cast<size_t>(idx);

        if (m_commonHeaderPresent.test(i)) {
            // 已存在：追加（用逗号分隔，符合 RFC 7230）
            m_commonHeaders[i] += ", ";
            m_commonHeaders[i] += value;
        } else {
            // 首次设置
            m_commonHeaders[i] = std::move(value);
            m_commonHeaderPresent.set(i);
        }
    }

    std::string_view HeaderPair::getCommonHeader(CommonHeaderIndex idx) const
    {
        size_t i = static_cast<size_t>(idx);
        if (m_commonHeaderPresent.test(i)) {
            return m_commonHeaders[i];
        }
        return {};
    }

    bool HeaderPair::hasCommonHeader(CommonHeaderIndex idx) const
    {
        return m_commonHeaderPresent.test(static_cast<size_t>(idx));
    }

    void HeaderPair::forEachHeader(std::function<void(std::string_view, std::string_view)> callback) const
    {
        if (m_mode == Mode::ServerSide) {
            // 先遍历常见 headers
            for (size_t i = 0; i < 15; ++i) {
                if (m_commonHeaderPresent.test(i)) {
                    callback(getCommonHeaderName(static_cast<CommonHeaderIndex>(i)),
                            m_commonHeaders[i]);
                }
            }
        }
        // 再遍历 map
        for (const auto& [k, v] : m_headerPairs) {
            callback(k, v);
        }
    }

    HttpMethod& HttpRequestHeader::method()
    {
        return this->m_method;
    }

    std::string& HttpRequestHeader::uri()
    {
        return this->m_uri;
    }

    HttpVersion& HttpRequestHeader::version()
    {
        return this->m_version;
    }

    std::map<std::string,std::string>& HttpRequestHeader::args()
    {
        return this->m_argList;
    }


    HeaderPair& HttpRequestHeader::headerPairs()
    {
        return this->m_headerPairs;
    }

    void HttpRequestHeader::commitParsedHeaderPair()
    {
        if (m_headerPairs.mode() == HeaderPair::Mode::ServerSide) {
            // Server 端：使用 fast path
            if (m_currentCommonHeaderIdx != CommonHeaderIndex::NotCommon) {
                m_headerPairs.setCommonHeader(
                    m_currentCommonHeaderIdx,
                    std::move(m_parseHeaderValue)
                );
            } else {
                // 罕见 header，key 已经是小写
                m_headerPairs.addNormalizedHeaderPair(
                    std::move(m_parseHeaderKey),
                    std::move(m_parseHeaderValue)
                );
            }
        } else {
            // Client 端：直接存 map，不转小写
            m_headerPairs.addHeaderPair(m_parseHeaderKey, m_parseHeaderValue);
        }

        m_parseHeaderKey.clear();
        m_parseHeaderValue.clear();
        m_currentCommonHeaderIdx = CommonHeaderIndex::NotCommon;
    }

    HttpErrorCode HttpRequestHeader::parseChar(char c)
    {
        switch (m_parseState) {
        case RequestParseState::Method:
            if (c == ' ') {
                m_method = stringToHttpMethod(m_parseMethodStr);
                m_parseState = RequestParseState::MethodSP;
            } else if (c == '\r' || c == '\n') {
                return kBadRequest;
            } else {
                m_parseMethodStr += c;
            }
            break;

        case RequestParseState::MethodSP:
            if (c == ' ') {
                // skip extra spaces
            } else if (c == '\r' || c == '\n') {
                return kBadRequest;
            } else {
                m_parseUriStr += c;
                m_parseState = RequestParseState::Uri;
            }
            break;

        case RequestParseState::Uri:
            if (c == ' ') {
                std::string full_uri = convertFromUri(m_parseUriStr, false);
                parseArgs(full_uri);
                if (m_uri.empty()) {
                    m_uri = full_uri;
                }
                m_parseState = RequestParseState::UriSP;
            } else if (c == '\r' || c == '\n') {
                return kBadRequest;
            } else {
                m_parseUriStr += c;
            }
            break;

        case RequestParseState::UriSP:
            if (c == ' ') {
                // skip extra spaces
            } else if (c == '\r' || c == '\n') {
                return kBadRequest;
            } else {
                m_parseVersionStr += c;
                m_parseState = RequestParseState::Version;
            }
            break;

        case RequestParseState::Version:
            if (c == '\r') {
                m_version = stringToHttpVersion(m_parseVersionStr);
                if (m_version == HttpVersion::HttpVersion_Unknown) {
                    return kVersionNotSupport;
                }
                // 只支持 HTTP/1.0 和 HTTP/1.1
                if (m_version != HttpVersion::HttpVersion_1_0 && m_version != HttpVersion::HttpVersion_1_1) {
                    return kVersionNotSupport;
                }
                m_parseState = RequestParseState::VersionCR;
            } else if (c == '\n') {
                return kBadRequest;
            } else {
                m_parseVersionStr += c;
            }
            break;

        case RequestParseState::VersionCR:
            if (c == '\n') {
                m_parseState = RequestParseState::VersionLF;
            } else {
                return kBadRequest;
            }
            break;

        case RequestParseState::VersionLF:
            if (c == '\r') {
                m_parseState = RequestParseState::HeaderEndCR;
            } else if (c == '\n') {
                return kBadRequest;
            } else {
                m_parseHeaderKey += normalizeHeaderKeyChar(c, m_headerPairs.mode());
                m_parseState = RequestParseState::HeaderKey;
            }
            break;

        case RequestParseState::HeaderKey:
            if (c == ':') {
                if (m_parseHeaderKey.size() > 256) {
                    return kBadRequest;
                }
                // Server 端：尝试匹配常见 header
                if (m_headerPairs.mode() == HeaderPair::Mode::ServerSide) {
                    m_currentCommonHeaderIdx = matchCommonHeader(m_parseHeaderKey);
                }
                m_parseState = RequestParseState::HeaderColon;
            } else if (c == '\r' || c == '\n') {
                return kBadRequest;
            } else {
                if (m_parseHeaderKey.size() >= 256) {
                    return kBadRequest;
                }
                // Server 端：边解析边转小写
                if (m_headerPairs.mode() == HeaderPair::Mode::ServerSide) {
                    m_parseHeaderKey += toLowerAsciiChar(c);
                } else {
                    m_parseHeaderKey += normalizeHeaderKeyChar(c, m_headerPairs.mode());
                }
            }
            break;

        case RequestParseState::HeaderColon:
            if (c == ' ') {
                m_parseState = RequestParseState::HeaderSpace;
            } else if (c == '\r') {
                commitParsedHeaderPair();
                m_parseState = RequestParseState::HeaderCR;
            } else {
                m_parseHeaderValue += c;
                m_parseState = RequestParseState::HeaderValue;
            }
            break;

        case RequestParseState::HeaderSpace:
            if (c == ' ') {
                // skip extra spaces
            } else if (c == '\r') {
                commitParsedHeaderPair();
                m_parseState = RequestParseState::HeaderCR;
            } else {
                m_parseHeaderValue += c;
                m_parseState = RequestParseState::HeaderValue;
            }
            break;

        case RequestParseState::HeaderValue:
            if (c == '\r') {
                commitParsedHeaderPair();
                m_parseState = RequestParseState::HeaderCR;
            } else {
                m_parseHeaderValue += c;
            }
            break;

        case RequestParseState::HeaderCR:
            if (c == '\n') {
                m_parseState = RequestParseState::HeaderLF;
            } else {
                return kBadRequest;
            }
            break;

        case RequestParseState::HeaderLF:
            if (c == '\r') {
                m_parseState = RequestParseState::HeaderEndCR;
            } else {
                m_parseHeaderKey += normalizeHeaderKeyChar(c, m_headerPairs.mode());
                m_parseState = RequestParseState::HeaderKey;
            }
            break;

        case RequestParseState::HeaderEndCR:
            if (c == '\n') {
                m_parseState = RequestParseState::Done;
                return kIncomplete; // 解析完成
            } else {
                return kBadRequest;
            }
            break;

        case RequestParseState::Done:
            return kIncomplete;
        }
        return kNoError;
    }

    std::pair<HttpErrorCode, ssize_t> HttpRequestHeader::fromString(std::string_view str)
    {
        if (m_parseState == RequestParseState::Done) {
            return {kNoError, 0};
        }
        ssize_t consumed = 0;
        for (char c : str) {
            HttpErrorCode err = parseChar(c);
            ++consumed;
            if (err == kIncomplete) {
                return {kNoError, consumed}; // 解析完成
            } else if (err != kNoError) {
                return {err, -1}; // 解析错误
            }
        }
        return {kNoError, 0}; // 数据不完整
    }

    std::pair<HttpErrorCode, ssize_t> HttpRequestHeader::fromIOVec(const std::vector<iovec>& iovecs)
    {
        if (m_parseState == RequestParseState::Done) {
            return {kNoError, 0};
        }

        auto appendHeaderKeyChunk = [&](const char* begin, size_t len) {
            if (len == 0) {
                return;
            }
            reserveIfUnset(m_parseHeaderKey, 32);
            if (m_headerPairs.mode() == HeaderPair::Mode::ServerSide) {
                const size_t old_size = m_parseHeaderKey.size();
                m_parseHeaderKey.resize(old_size + len);
                for (size_t i = 0; i < len; ++i) {
                    m_parseHeaderKey[old_size + i] = toLowerAsciiChar(begin[i]);
                }
                return;
            }
            m_parseHeaderKey.append(begin, len);
        };

        // 调用方保证每次传入的buffer都是新数据（已consume过的）
        size_t total_consumed = 0;
        for (size_t iov_idx = 0; iov_idx < iovecs.size(); ++iov_idx) {
            const char* data = static_cast<const char*>(iovecs[iov_idx].iov_base);
            const size_t len = iovecs[iov_idx].iov_len;
            size_t i = 0;

            while (i < len) {
                switch (m_parseState) {
                case RequestParseState::Method: {
                    const size_t start = i;
                    while (i < len && data[i] != ' ' && data[i] != '\r' && data[i] != '\n') {
                        ++i;
                    }
                    if (i > start) {
                        reserveIfUnset(m_parseMethodStr, 16);
                        m_parseMethodStr.append(data + start, i - start);
                    }
                    if (i == len) {
                        break;
                    }

                    const char c = data[i++];
                    if (c != ' ') {
                        return {kBadRequest, -1};
                    }
                    m_method = stringToHttpMethod(m_parseMethodStr);
                    m_parseState = RequestParseState::MethodSP;
                    break;
                }

                case RequestParseState::MethodSP: {
                    while (i < len && data[i] == ' ') {
                        ++i;
                    }
                    if (i == len) {
                        break;
                    }

                    const char c = data[i++];
                    if (c == '\r' || c == '\n') {
                        return {kBadRequest, -1};
                    }
                    reserveIfUnset(m_parseUriStr, 64);
                    m_parseUriStr.push_back(c);
                    m_parseState = RequestParseState::Uri;
                    break;
                }

                case RequestParseState::Uri: {
                    const size_t start = i;
                    while (i < len && data[i] != ' ' && data[i] != '\r' && data[i] != '\n') {
                        ++i;
                    }
                    if (i > start) {
                        reserveIfUnset(m_parseUriStr, 64);
                        m_parseUriStr.append(data + start, i - start);
                    }
                    if (i == len) {
                        break;
                    }

                    const char c = data[i++];
                    if (c != ' ') {
                        return {kBadRequest, -1};
                    }

                    std::string full_uri = convertFromUri(m_parseUriStr, false);
                    parseArgs(full_uri);
                    if (m_uri.empty()) {
                        m_uri = full_uri;
                    }
                    m_parseState = RequestParseState::UriSP;
                    break;
                }

                case RequestParseState::UriSP: {
                    while (i < len && data[i] == ' ') {
                        ++i;
                    }
                    if (i == len) {
                        break;
                    }

                    const char c = data[i++];
                    if (c == '\r' || c == '\n') {
                        return {kBadRequest, -1};
                    }
                    reserveIfUnset(m_parseVersionStr, 16);
                    m_parseVersionStr.push_back(c);
                    m_parseState = RequestParseState::Version;
                    break;
                }

                case RequestParseState::Version: {
                    const size_t start = i;
                    while (i < len && data[i] != '\r' && data[i] != '\n') {
                        ++i;
                    }
                    if (i > start) {
                        reserveIfUnset(m_parseVersionStr, 16);
                        m_parseVersionStr.append(data + start, i - start);
                    }
                    if (i == len) {
                        break;
                    }

                    const char c = data[i++];
                    if (c == '\n') {
                        return {kBadRequest, -1};
                    }

                    m_version = stringToHttpVersion(m_parseVersionStr);
                    if (m_version == HttpVersion::HttpVersion_Unknown) {
                        return {kVersionNotSupport, -1};
                    }
                    if (m_version != HttpVersion::HttpVersion_1_0 &&
                        m_version != HttpVersion::HttpVersion_1_1) {
                        return {kVersionNotSupport, -1};
                    }
                    m_parseState = RequestParseState::VersionCR;
                    break;
                }

                case RequestParseState::VersionCR:
                    if (i == len) {
                        break;
                    }
                    if (data[i++] != '\n') {
                        return {kBadRequest, -1};
                    }
                    m_parseState = RequestParseState::VersionLF;
                    break;

                case RequestParseState::VersionLF:
                    if (i == len) {
                        break;
                    }
                    if (data[i] == '\r') {
                        ++i;
                        m_parseState = RequestParseState::HeaderEndCR;
                        break;
                    }
                    if (data[i] == '\n') {
                        return {kBadRequest, -1};
                    }
                    reserveIfUnset(m_parseHeaderKey, 32);
                    m_parseHeaderKey.push_back(normalizeHeaderKeyChar(data[i], m_headerPairs.mode()));
                    ++i;
                    m_parseState = RequestParseState::HeaderKey;
                    break;

                case RequestParseState::HeaderKey: {
                    const size_t start = i;
                    while (i < len && data[i] != ':' && data[i] != '\r' && data[i] != '\n') {
                        ++i;
                    }
                    appendHeaderKeyChunk(data + start, i - start);
                    if (i == len) {
                        break;
                    }
                    if (data[i++] != ':') {
                        return {kBadRequest, -1};
                    }
                    if (m_parseHeaderKey.size() > 256) {
                        return {kBadRequest, -1};
                    }
                    // Server 端：尝试匹配常见 header
                    if (m_headerPairs.mode() == HeaderPair::Mode::ServerSide) {
                        m_currentCommonHeaderIdx = matchCommonHeader(m_parseHeaderKey);
                    }
                    m_parseState = RequestParseState::HeaderColon;
                    break;
                }

                case RequestParseState::HeaderColon:
                    if (i == len) {
                        break;
                    }
                    if (data[i] == ' ') {
                        ++i;
                        m_parseState = RequestParseState::HeaderSpace;
                        break;
                    }
                    if (data[i] == '\r') {
                        ++i;
                        commitParsedHeaderPair();
                        m_parseState = RequestParseState::HeaderCR;
                        break;
                    }
                    reserveIfUnset(m_parseHeaderValue, 64);
                    m_parseHeaderValue.push_back(data[i]);
                    ++i;
                    m_parseState = RequestParseState::HeaderValue;
                    break;

                case RequestParseState::HeaderSpace:
                    while (i < len && data[i] == ' ') {
                        ++i;
                    }
                    if (i == len) {
                        break;
                    }
                    if (data[i] == '\r') {
                        ++i;
                        commitParsedHeaderPair();
                        m_parseState = RequestParseState::HeaderCR;
                        break;
                    }
                    reserveIfUnset(m_parseHeaderValue, 64);
                    m_parseHeaderValue.push_back(data[i]);
                    ++i;
                    m_parseState = RequestParseState::HeaderValue;
                    break;

                case RequestParseState::HeaderValue: {
                    const size_t start = i;
                    while (i < len && data[i] != '\r') {
                        ++i;
                    }
                    if (i > start) {
                        reserveIfUnset(m_parseHeaderValue, 64);
                        m_parseHeaderValue.append(data + start, i - start);
                    }
                    if (i == len) {
                        break;
                    }
                    ++i; // consume '\r'
                    commitParsedHeaderPair();
                    m_parseState = RequestParseState::HeaderCR;
                    break;
                }

                case RequestParseState::HeaderCR:
                    if (i == len) {
                        break;
                    }
                    if (data[i++] != '\n') {
                        return {kBadRequest, -1};
                    }
                    m_parseState = RequestParseState::HeaderLF;
                    break;

                case RequestParseState::HeaderLF:
                    if (i == len) {
                        break;
                    }
                    if (data[i] == '\r') {
                        ++i;
                        m_parseState = RequestParseState::HeaderEndCR;
                        break;
                    }
                    reserveIfUnset(m_parseHeaderKey, 32);
                    m_parseHeaderKey.push_back(normalizeHeaderKeyChar(data[i], m_headerPairs.mode()));
                    ++i;
                    m_parseState = RequestParseState::HeaderKey;
                    break;

                case RequestParseState::HeaderEndCR:
                    if (i == len) {
                        break;
                    }
                    if (data[i++] != '\n') {
                        return {kBadRequest, -1};
                    }
                    m_parseState = RequestParseState::Done;
                    total_consumed += i;
                    m_parsedBytes += i;
                    return {kNoError, static_cast<ssize_t>(total_consumed)};

                case RequestParseState::Done:
                    total_consumed += i;
                    m_parsedBytes += i;
                    return {kNoError, static_cast<ssize_t>(total_consumed)};
                }
            }

            total_consumed += i;
            m_parsedBytes += i;
        }
        return {kIncomplete, static_cast<ssize_t>(total_consumed)};
    }

    std::string HttpRequestHeader::toString() const
    {
        // 构建 URI（带参数）
        std::string uri_str = m_uri;
        if (!m_argList.empty())
        {
            uri_str += '?';
            int i = 0;
            for (const auto& [k, v] : m_argList)
            {
                uri_str += k;
                uri_str += '=';
                uri_str += v;
                if(i++ < static_cast<int>(m_argList.size()) - 1) {
                    uri_str += '&';
                }
            }
        }
        uri_str = convertToUri(std::move(uri_str));
        
        // 获取方法、版本和头部字符串
        std::string method_str = httpMethodToString(this->m_method);
        std::string version_str = httpVersionToString(this->m_version);
        const size_t headers_size = m_headerPairs.estimatedSerializedSize();

        // 预分配结果字符串
        size_t estimated_size = method_str.size() + 1 + uri_str.size() + 1 +
                                version_str.size() + 2 + headers_size + 2;
        std::string result;
        result.reserve(estimated_size);
        
        // 直接拼接，避免 ostringstream 开销
        result += method_str;
        result += ' ';
        result += uri_str;
        result += ' ';
        result += version_str;
        result += "\r\n";
        m_headerPairs.appendTo(result);
        result += "\r\n";
        
        return result;
    }

    bool HttpRequestHeader::isKeepAlive() const
    {
        const auto* conn = m_headerPairs.getValuePtr("connection");
        if (conn == nullptr || conn->empty()) {
            return m_version == HttpVersion::HttpVersion_1_1;
        }
        if (headerValueContainsToken(*conn, "close")) {
            return false;
        }
        if (headerValueContainsToken(*conn, "keep-alive")) {
            return true;
        }
        return m_version == HttpVersion::HttpVersion_1_1;
    }

    bool HttpRequestHeader::isChunked() const
    {
        if (const auto* te = m_headerPairs.getValuePtr("transfer-encoding"); te != nullptr) {
            return headerValueContainsToken(*te, "chunked");
        }
        return false;
    }

    bool HttpRequestHeader::isConnectionClose() const
    {
        if (const auto* conn = m_headerPairs.getValuePtr("connection"); conn != nullptr) {
            return headerValueContainsToken(*conn, "close");
        }
        return false;
    }

    void HttpRequestHeader::copyFrom(const HttpRequestHeader& header)
    {
        this->m_method = header.m_method;
        this->m_uri = header.m_uri;
        this->m_version = header.m_version;
        this->m_argList = header.m_argList;
        this->m_headerPairs = header.m_headerPairs;
    }

    void HttpRequestHeader::reset()
    {
        m_version = HttpVersion::HttpVersion_Unknown;
        m_method = HttpMethod::UNKNOWN;
        if(!m_uri.empty()) m_uri.clear();
        if(!m_argList.empty()) m_argList.clear();
        m_headerPairs.clear();
        // 重置解析状态
        m_parseState = RequestParseState::Method;
        m_parseMethodStr.clear();
        m_parseUriStr.clear();
        m_parseVersionStr.clear();
        m_parseHeaderKey.clear();
        m_parseHeaderValue.clear();
        m_parsedBytes = 0;
    }

    void HttpRequestHeader::parseArgs(std::string uri)
    {
        size_t argindx = uri.find('?');
        if (argindx != std::string::npos)
        {
            int cur = 0;
            this->m_uri = uri.substr(cur, argindx - cur);
            std::string args = uri.substr(argindx + 1);
            std::string key, value;
            int status = 0;
            for (int i = 0; i < args.length(); i++)
            {
                if (status == 0)
                {
                    if (args[i] == '=')
                    {
                        status = 1;
                    }
                    else
                    {
                        key += args[i];
                    }
                }
                else
                {
                    if (args[i] == '&')
                    {
                        this->m_argList[key] = value;
                        key = "", value = "";
                        status = 0;
                    }
                    else
                    {
                        value += args[i];
                        if (i == args.length() - 1)
                        {
                            this->m_argList[key] = value;
                        }
                    }
                }
            }
        }
    }

    std::string HttpRequestHeader::convertFromUri(std::string_view url, bool convert_plus_to_space)
    {
        std::string result;
        for (size_t i = 0; i < url.size(); i++)
        {
            if (url[i] == '%' && i + 1 < url.size())
            {
                if (url[i + 1] == 'u')
                {
                    auto val = 0;
                    if (fromHexToI(url, i + 2, 4, val))
                    {
                        char buff[4];
                        size_t len = toUtf8(val, buff);
                        if (len > 0)
                        {
                            result.append(buff, len);
                        }
                        i += 5;
                    }
                    else
                    {
                        result += url[i];
                    }
                }
                else
                {
                    auto val = 0;
                    if (fromHexToI(url, i + 1, 2, val))
                    {
                        result += static_cast<char>(val);
                        i += 2;
                    }
                    else
                    {
                        result += url[i];
                    }
                }
            }
            else if (convert_plus_to_space && url[i] == '+')
            {
                result += ' ';
            }
            else
            {
                result += url[i];
            }
        }

        return result;
    }

    std::string HttpRequestHeader::convertToUri(std::string&& url) const
    {
        std::string result;
        size_t n = url.size();
        for (size_t i = 0; i < n ; i++)
        {
            switch (url[i])
            {
            case ' ':
                result += "%20";
                break;
            case '+':
                result += "%2B";
                break;
            case '\r':
                result += "%0D";
                break;
            case '\n':
                result += "%0A";
                break;
            case '\'':
                result += "%27";
                break;
            case ',':
                result += "%2C";
                break;
                // case ':': result += "%3A"; break; // ok? probably...
            case ';':
                result += "%3B";
                break;
            default:
                auto c = static_cast<uint8_t>(url[i]);
                if (c >= 0x80)
                {
                    result += '%';
                    char hex[4];
                    auto len = snprintf(hex, sizeof(hex) - 1, "%02X", c);
                    if (len == 2) {
                        result.append(hex, static_cast<size_t>(len));
                    }
                }
                else
                {
                    result += url[i];
                }
                break;
            }
        }

        return std::move(result);

    }

    bool HttpRequestHeader::isHex(const char c, int& v)
    {
        if (0x20 <= c && isdigit(c))
        {
            v = c - '0';
            return true;
        }
        else if ('A' <= c && c <= 'F')
        {
            v = c - 'A' + 10;
            return true;
        }
        else if ('a' <= c && c <= 'f')
        {
            v = c - 'a' + 10;
            return true;
        }
        return false;
    }

    size_t HttpRequestHeader::toUtf8(const int code, char* buff)
    {
        if (code < 0x0080)
        {
            buff[0] = static_cast<char>(code & 0x7F);
            return 1;
        }
        else if (code < 0x0800)
        {
            buff[0] = static_cast<char>(0xC0 | ((code >> 6) & 0x1F));
            buff[1] = static_cast<char>(0x80 | (code & 0x3F));
            return 2;
        }
        else if (code < 0xD800)
        {
            buff[0] = static_cast<char>(0xE0 | ((code >> 12) & 0xF));
            buff[1] = static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            buff[2] = static_cast<char>(0x80 | (code & 0x3F));
            return 3;
        }
        else if (code < 0xE000)
        {
            return 0;
        }
        else if (code < 0x10000)
        {
            buff[0] = static_cast<char>(0xE0 | ((code >> 12) & 0xF));
            buff[1] = static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            buff[2] = static_cast<char>(0x80 | (code & 0x3F));
            return 3;
        }
        else if (code < 0x110000)
        {
            buff[0] = static_cast<char>(0xF0 | ((code >> 18) & 0x7));
            buff[1] = static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            buff[2] = static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            buff[3] = static_cast<char>(0x80 | (code & 0x3F));
            return 4;
        }
        return 0;
    }

    bool HttpRequestHeader::fromHexToI(const std::string_view& s, size_t i, size_t cnt, int& val)
    {
        if (i >= s.size())
        {
            return false;
        }

        val = 0;
        for (; cnt; i++, --cnt)
        {
            if (!s[i])
            {
                return false;
            }
            auto v = 0;
            if (isHex(s[i], v))
            {
                val = val * 16 + v;
            }
            else
            {
                return false;
            }
        }
        return true;
    }

    HttpVersion& HttpResponseHeader::version()
    {
        return this->m_version;
    }

    HttpStatusCode& HttpResponseHeader::code()
    {
        return this->m_code;
    }

    HeaderPair& HttpResponseHeader::headerPairs()
    {
        return this->m_headerPairs;
    }

    void HttpResponseHeader::commitParsedHeaderPair()
    {
        if (m_headerPairs.mode() == HeaderPair::Mode::ServerSide) {
            // Server 端：使用 fast path
            if (m_currentCommonHeaderIdx != CommonHeaderIndex::NotCommon) {
                m_headerPairs.setCommonHeader(
                    m_currentCommonHeaderIdx,
                    std::move(m_parseHeaderValue)
                );
            } else {
                // 罕见 header，key 已经是小写
                m_headerPairs.addNormalizedHeaderPair(
                    std::move(m_parseHeaderKey),
                    std::move(m_parseHeaderValue)
                );
            }
        } else {
            // Client 端：直接存 map，不转小写
            m_headerPairs.addHeaderPair(m_parseHeaderKey, m_parseHeaderValue);
        }

        m_parseHeaderKey.clear();
        m_parseHeaderValue.clear();
        m_currentCommonHeaderIdx = CommonHeaderIndex::NotCommon;
    }

    bool HttpResponseHeader::isKeepAlive() const
    {
        const auto* conn = m_headerPairs.getValuePtr("connection");
        if (conn == nullptr || conn->empty()) {
            return m_version == HttpVersion::HttpVersion_1_1;
        }
        if (headerValueContainsToken(*conn, "close")) {
            return false;
        }
        if (headerValueContainsToken(*conn, "keep-alive")) {
            return true;
        }
        return m_version == HttpVersion::HttpVersion_1_1;
    }

    bool HttpResponseHeader::isChunked() const
    {
        if (const auto* te = m_headerPairs.getValuePtr("transfer-encoding"); te != nullptr) {
            return headerValueContainsToken(*te, "chunked");
        }
        return false;
    }

    bool HttpResponseHeader::isConnectionClose() const
    {
        if (const auto* conn = m_headerPairs.getValuePtr("connection"); conn != nullptr) {
            return headerValueContainsToken(*conn, "close");
        }
        return false;
    }

    std::string HttpResponseHeader::toString() const
    {
        // 获取各部分字符串
        std::string version_str = httpVersionToString(m_version);
        std::string code_str = std::to_string(static_cast<int>(this->m_code));
        std::string status_str = httpStatusCodeToString(m_code);
        const size_t headers_size = m_headerPairs.estimatedSerializedSize();

        // 预分配结果字符串
        size_t estimated_size = version_str.size() + 1 + code_str.size() + 1 +
                                status_str.size() + 2 + headers_size + 2;
        std::string result;
        result.reserve(estimated_size);
        
        // 直接拼接，避免 ostringstream 开销
        result += version_str;
        result += ' ';
        result += code_str;
        result += ' ';
        result += status_str;
        result += "\r\n";
        m_headerPairs.appendTo(result);
        result += "\r\n";
        
        return result;
    }

    HttpErrorCode HttpResponseHeader::parseChar(char c)
    {
        switch (m_parseState) {
        case ResponseParseState::Version:
            if (c == ' ') {
                m_version = stringToHttpVersion(m_parseVersionStr);
                if (m_version != HttpVersion::HttpVersion_1_0 && m_version != HttpVersion::HttpVersion_1_1) {
                    return kVersionNotSupport;
                }
                m_parseState = ResponseParseState::VersionSP;
            } else if (c == '\r' || c == '\n') {
                return kBadRequest;
            } else {
                m_parseVersionStr += c;
            }
            break;

        case ResponseParseState::VersionSP:
            if (c == ' ') {
                // skip extra spaces
            } else if (c == '\r' || c == '\n') {
                return kBadRequest;
            } else {
                m_parseCodeStr += c;
                m_parseState = ResponseParseState::Code;
            }
            break;

        case ResponseParseState::Code:
            if (c == ' ') {
                if (!parseHttpStatusCode(m_parseCodeStr, m_code)) {
                    return kHttpCodeInvalid;
                }
                m_parseState = ResponseParseState::CodeSP;
            } else if (c == '\r') {
                // HTTP/1.1 200\r\n (no status text)
                if (!parseHttpStatusCode(m_parseCodeStr, m_code)) {
                    return kHttpCodeInvalid;
                }
                m_parseState = ResponseParseState::StatusCR;
            } else if (c == '\n') {
                return kBadRequest;
            } else {
                m_parseCodeStr += c;
            }
            break;

        case ResponseParseState::CodeSP:
            if (c == ' ') {
                // skip extra spaces
            } else if (c == '\r') {
                m_parseState = ResponseParseState::StatusCR;
            } else if (c == '\n') {
                return kBadRequest;
            } else {
                m_parseState = ResponseParseState::Status;
            }
            break;

        case ResponseParseState::Status:
            if (c == '\r') {
                m_parseState = ResponseParseState::StatusCR;
            } else if (c == '\n') {
                return kBadRequest;
            }
            // ignore status text
            break;

        case ResponseParseState::StatusCR:
            if (c == '\n') {
                m_parseState = ResponseParseState::StatusLF;
            } else {
                return kBadRequest;
            }
            break;

        case ResponseParseState::StatusLF:
            if (c == '\r') {
                m_parseState = ResponseParseState::HeaderEndCR;
            } else if (c == '\n') {
                return kBadRequest;
            } else {
                // Server 端：边解析边转小写
                if (m_headerPairs.mode() == HeaderPair::Mode::ServerSide) {
                    m_parseHeaderKey += toLowerAsciiChar(c);
                } else {
                    m_parseHeaderKey += normalizeHeaderKeyChar(c, m_headerPairs.mode());
                }
                m_parseState = ResponseParseState::HeaderKey;
            }
            break;

        case ResponseParseState::HeaderKey:
            if (c == ':') {
                if (m_parseHeaderKey.size() > 256) {
                    return kBadRequest;
                }
                // Server 端：尝试匹配常见 header
                if (m_headerPairs.mode() == HeaderPair::Mode::ServerSide) {
                    m_currentCommonHeaderIdx = matchCommonHeader(m_parseHeaderKey);
                }
                m_parseState = ResponseParseState::HeaderColon;
            } else if (c == '\r' || c == '\n') {
                return kBadRequest;
            } else {
                if (m_parseHeaderKey.size() >= 256) {
                    return kBadRequest;
                }
                // Server 端：边解析边转小写
                if (m_headerPairs.mode() == HeaderPair::Mode::ServerSide) {
                    m_parseHeaderKey += toLowerAsciiChar(c);
                } else {
                    m_parseHeaderKey += normalizeHeaderKeyChar(c, m_headerPairs.mode());
                }
            }
            break;

        case ResponseParseState::HeaderColon:
            if (c == ' ') {
                m_parseState = ResponseParseState::HeaderSpace;
            } else if (c == '\r') {
                commitParsedHeaderPair();
                m_parseState = ResponseParseState::HeaderCR;
            } else {
                m_parseHeaderValue += c;
                m_parseState = ResponseParseState::HeaderValue;
            }
            break;

        case ResponseParseState::HeaderSpace:
            if (c == ' ') {
                // skip extra spaces
            } else if (c == '\r') {
                commitParsedHeaderPair();
                m_parseState = ResponseParseState::HeaderCR;
            } else {
                m_parseHeaderValue += c;
                m_parseState = ResponseParseState::HeaderValue;
            }
            break;

        case ResponseParseState::HeaderValue:
            if (c == '\r') {
                commitParsedHeaderPair();
                m_parseState = ResponseParseState::HeaderCR;
            } else {
                m_parseHeaderValue += c;
            }
            break;

        case ResponseParseState::HeaderCR:
            if (c == '\n') {
                m_parseState = ResponseParseState::HeaderLF;
            } else {
                return kBadRequest;
            }
            break;

        case ResponseParseState::HeaderLF:
            if (c == '\r') {
                m_parseState = ResponseParseState::HeaderEndCR;
            } else {
                // Server 端：边解析边转小写
                if (m_headerPairs.mode() == HeaderPair::Mode::ServerSide) {
                    m_parseHeaderKey += toLowerAsciiChar(c);
                } else {
                    m_parseHeaderKey += normalizeHeaderKeyChar(c, m_headerPairs.mode());
                }
                m_parseState = ResponseParseState::HeaderKey;
            }
            break;

        case ResponseParseState::HeaderEndCR:
            if (c == '\n') {
                m_parseState = ResponseParseState::Done;
                return kIncomplete; // 解析完成
            } else {
                return kBadRequest;
            }
            break;

        case ResponseParseState::Done:
            return kIncomplete;
        }
        return kNoError;
    }

    std::pair<HttpErrorCode, ssize_t> HttpResponseHeader::fromString(std::string_view str)
    {
        if (m_parseState == ResponseParseState::Done) {
            return {kNoError, 0};
        }
        ssize_t consumed = 0;
        for (char c : str) {
            HttpErrorCode err = parseChar(c);
            ++consumed;
            if (err == kIncomplete) {
                return {kNoError, consumed}; // 解析完成
            } else if (err != kNoError) {
                return {err, -1}; // 解析错误
            }
        }
        return {kNoError, 0}; // 数据不完整
    }

    std::pair<HttpErrorCode, ssize_t> HttpResponseHeader::fromIOVec(const std::vector<iovec>& iovecs)
    {
        if (m_parseState == ResponseParseState::Done) {
            return {kNoError, 0};
        }

        auto appendHeaderKeyChunk = [&](const char* begin, size_t len) {
            if (len == 0) {
                return;
            }
            reserveIfUnset(m_parseHeaderKey, 32);
            if (m_headerPairs.mode() == HeaderPair::Mode::ServerSide) {
                const size_t old_size = m_parseHeaderKey.size();
                m_parseHeaderKey.resize(old_size + len);
                for (size_t i = 0; i < len; ++i) {
                    m_parseHeaderKey[old_size + i] = toLowerAsciiChar(begin[i]);
                }
                return;
            }
            m_parseHeaderKey.append(begin, len);
        };

        auto parseStatusCode = [&]() -> bool {
            if (m_parseCodeStr.empty()) {
                return false;
            }

            int code = 0;
            const char* begin = m_parseCodeStr.data();
            const char* end = begin + m_parseCodeStr.size();
            auto [ptr, ec] = std::from_chars(begin, end, code);
            if (ec != std::errc{} || ptr != end) {
                return false;
            }

            m_code = static_cast<HttpStatusCode>(code);
            return true;
        };

        // 调用方保证每次传入的buffer都是新数据（已consume过的）
        size_t total_consumed = 0;
        for (size_t iov_idx = 0; iov_idx < iovecs.size(); ++iov_idx) {
            const char* data = static_cast<const char*>(iovecs[iov_idx].iov_base);
            const size_t len = iovecs[iov_idx].iov_len;
            size_t i = 0;

            while (i < len) {
                switch (m_parseState) {
                case ResponseParseState::Version: {
                    const size_t start = i;
                    while (i < len && data[i] != ' ' && data[i] != '\r' && data[i] != '\n') {
                        ++i;
                    }
                    if (i > start) {
                        reserveIfUnset(m_parseVersionStr, 16);
                        m_parseVersionStr.append(data + start, i - start);
                    }
                    if (i == len) {
                        break;
                    }

                    const char c = data[i++];
                    if (c != ' ') {
                        return {kBadRequest, -1};
                    }
                    m_version = stringToHttpVersion(m_parseVersionStr);
                    if (m_version != HttpVersion::HttpVersion_1_0 && m_version != HttpVersion::HttpVersion_1_1) {
                        return {kVersionNotSupport, -1};
                    }
                    m_parseState = ResponseParseState::VersionSP;
                    break;
                }

                case ResponseParseState::VersionSP: {
                    while (i < len && data[i] == ' ') {
                        ++i;
                    }
                    if (i == len) {
                        break;
                    }

                    const char c = data[i++];
                    if (c == '\r' || c == '\n') {
                        return {kBadRequest, -1};
                    }
                    reserveIfUnset(m_parseCodeStr, 4);
                    m_parseCodeStr.push_back(c);
                    m_parseState = ResponseParseState::Code;
                    break;
                }

                case ResponseParseState::Code: {
                    const size_t start = i;
                    while (i < len && data[i] != ' ' && data[i] != '\r' && data[i] != '\n') {
                        ++i;
                    }
                    if (i > start) {
                        reserveIfUnset(m_parseCodeStr, 4);
                        m_parseCodeStr.append(data + start, i - start);
                    }
                    if (i == len) {
                        break;
                    }

                    const char c = data[i++];
                    if (c == '\n') {
                        return {kBadRequest, -1};
                    }
                    if (!parseStatusCode()) {
                        return {kHttpCodeInvalid, -1};
                    }
                    m_parseState = (c == '\r') ? ResponseParseState::StatusCR
                                               : ResponseParseState::CodeSP;
                    break;
                }

                case ResponseParseState::CodeSP:
                    while (i < len && data[i] == ' ') {
                        ++i;
                    }
                    if (i == len) {
                        break;
                    }
                    if (data[i] == '\r') {
                        ++i;
                        m_parseState = ResponseParseState::StatusCR;
                        break;
                    }
                    if (data[i] == '\n') {
                        return {kBadRequest, -1};
                    }
                    ++i; // consume first status-text char (ignored)
                    m_parseState = ResponseParseState::Status;
                    break;

                case ResponseParseState::Status:
                    while (i < len && data[i] != '\r' && data[i] != '\n') {
                        ++i;
                    }
                    if (i == len) {
                        break;
                    }
                    if (data[i++] == '\n') {
                        return {kBadRequest, -1};
                    }
                    m_parseState = ResponseParseState::StatusCR;
                    break;

                case ResponseParseState::StatusCR:
                    if (i == len) {
                        break;
                    }
                    if (data[i++] != '\n') {
                        return {kBadRequest, -1};
                    }
                    m_parseState = ResponseParseState::StatusLF;
                    break;

                case ResponseParseState::StatusLF:
                    if (i == len) {
                        break;
                    }
                    if (data[i] == '\r') {
                        ++i;
                        m_parseState = ResponseParseState::HeaderEndCR;
                        break;
                    }
                    if (data[i] == '\n') {
                        return {kBadRequest, -1};
                    }
                    reserveIfUnset(m_parseHeaderKey, 32);
                    // Server 端：边解析边转小写
                    if (m_headerPairs.mode() == HeaderPair::Mode::ServerSide) {
                        m_parseHeaderKey.push_back(toLowerAsciiChar(data[i]));
                    } else {
                        m_parseHeaderKey.push_back(normalizeHeaderKeyChar(data[i], m_headerPairs.mode()));
                    }
                    ++i;
                    m_parseState = ResponseParseState::HeaderKey;
                    break;

                case ResponseParseState::HeaderKey: {
                    const size_t start = i;
                    while (i < len && data[i] != ':' && data[i] != '\r' && data[i] != '\n') {
                        ++i;
                    }
                    appendHeaderKeyChunk(data + start, i - start);
                    if (i == len) {
                        break;
                    }
                    if (data[i++] != ':') {
                        return {kBadRequest, -1};
                    }
                    if (m_parseHeaderKey.size() > 256) {
                        return {kBadRequest, -1};
                    }
                    // Server 端：尝试匹配常见 header
                    if (m_headerPairs.mode() == HeaderPair::Mode::ServerSide) {
                        m_currentCommonHeaderIdx = matchCommonHeader(m_parseHeaderKey);
                    }
                    m_parseState = ResponseParseState::HeaderColon;
                    break;
                }

                case ResponseParseState::HeaderColon:
                    if (i == len) {
                        break;
                    }
                    if (data[i] == ' ') {
                        ++i;
                        m_parseState = ResponseParseState::HeaderSpace;
                        break;
                    }
                    if (data[i] == '\r') {
                        ++i;
                        commitParsedHeaderPair();
                        m_parseState = ResponseParseState::HeaderCR;
                        break;
                    }
                    reserveIfUnset(m_parseHeaderValue, 64);
                    m_parseHeaderValue.push_back(data[i]);
                    ++i;
                    m_parseState = ResponseParseState::HeaderValue;
                    break;

                case ResponseParseState::HeaderSpace:
                    while (i < len && data[i] == ' ') {
                        ++i;
                    }
                    if (i == len) {
                        break;
                    }
                    if (data[i] == '\r') {
                        ++i;
                        commitParsedHeaderPair();
                        m_parseState = ResponseParseState::HeaderCR;
                        break;
                    }
                    reserveIfUnset(m_parseHeaderValue, 64);
                    m_parseHeaderValue.push_back(data[i]);
                    ++i;
                    m_parseState = ResponseParseState::HeaderValue;
                    break;

                case ResponseParseState::HeaderValue: {
                    const size_t start = i;
                    while (i < len && data[i] != '\r') {
                        ++i;
                    }
                    if (i > start) {
                        reserveIfUnset(m_parseHeaderValue, 64);
                        m_parseHeaderValue.append(data + start, i - start);
                    }
                    if (i == len) {
                        break;
                    }
                    ++i; // consume '\r'
                    commitParsedHeaderPair();
                    m_parseState = ResponseParseState::HeaderCR;
                    break;
                }

                case ResponseParseState::HeaderCR:
                    if (i == len) {
                        break;
                    }
                    if (data[i++] != '\n') {
                        return {kBadRequest, -1};
                    }
                    m_parseState = ResponseParseState::HeaderLF;
                    break;

                case ResponseParseState::HeaderLF:
                    if (i == len) {
                        break;
                    }
                    if (data[i] == '\r') {
                        ++i;
                        m_parseState = ResponseParseState::HeaderEndCR;
                        break;
                    }
                    reserveIfUnset(m_parseHeaderKey, 32);
                    // Server 端：边解析边转小写
                    if (m_headerPairs.mode() == HeaderPair::Mode::ServerSide) {
                        m_parseHeaderKey.push_back(toLowerAsciiChar(data[i]));
                    } else {
                        m_parseHeaderKey.push_back(normalizeHeaderKeyChar(data[i], m_headerPairs.mode()));
                    }
                    ++i;
                    m_parseState = ResponseParseState::HeaderKey;
                    break;

                case ResponseParseState::HeaderEndCR:
                    if (i == len) {
                        break;
                    }
                    if (data[i++] != '\n') {
                        return {kBadRequest, -1};
                    }
                    m_parseState = ResponseParseState::Done;
                    total_consumed += i;
                    m_parsedBytes += i;
                    return {kNoError, static_cast<ssize_t>(total_consumed)};

                case ResponseParseState::Done:
                    total_consumed += i;
                    m_parsedBytes += i;
                    return {kNoError, static_cast<ssize_t>(total_consumed)};
                }
            }

            total_consumed += i;
            m_parsedBytes += i;
        }
        return {kIncomplete, static_cast<ssize_t>(total_consumed)};
    }

    void HttpResponseHeader::reset()
    {
        m_code = static_cast<HttpStatusCode>(0);
        m_version = HttpVersion::HttpVersion_Unknown;
        m_headerPairs.clear();
        // 重置解析状态
        m_parseState = ResponseParseState::Version;
        m_parseVersionStr.clear();
        m_parseCodeStr.clear();
        m_parseHeaderKey.clear();
        m_parseHeaderValue.clear();
        m_parsedBytes = 0;
    }

    void HttpResponseHeader::copyFrom(const HttpResponseHeader &header)
    {
        this->m_code = header.m_code;
        this->m_version = header.m_version;
        this->m_headerPairs = header.m_headerPairs;
    }
}
