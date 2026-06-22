#include "redis_protocol.h"
#include <cstring>
#include <charconv>
#include <limits>

#if defined(__SSE2__)
#include <immintrin.h>
#endif
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace galay::redis::protocol
{
    namespace
    {
        constexpr int64_t kMaxRespBulkLength = 512LL * 1024LL * 1024LL;
        constexpr int64_t kMaxRespAggregateLength = 512LL * 1024LL * 1024LL;
        constexpr int64_t kMaxRespMapPairs = kMaxRespAggregateLength / 2;

        const char* findCRLFSimd(const char* begin, const char* end)
        {
#if defined(__SSE2__)
            const __m128i cr = _mm_set1_epi8('\r');
            const char* p = begin;
            while (p + 16 <= end) {
                const __m128i block = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
                int mask = _mm_movemask_epi8(_mm_cmpeq_epi8(block, cr));
                while (mask != 0) {
                    const int idx = __builtin_ctz(mask);
                    const char* candidate = p + idx;
                    if (candidate + 1 < end && candidate[1] == '\n') {
                        return candidate;
                    }
                    mask &= (mask - 1);
                }
                p += 16;
            }
            return p;
#elif defined(__ARM_NEON)
            const uint8x16_t cr = vdupq_n_u8(static_cast<uint8_t>('\r'));
            const char* p = begin;
            while (p + 16 <= end) {
                const uint8x16_t block = vld1q_u8(reinterpret_cast<const uint8_t*>(p));
                const uint8x16_t eq = vceqq_u8(block, cr);
                const uint64x2_t lanes = vreinterpretq_u64_u8(eq);
                if ((vgetq_lane_u64(lanes, 0) | vgetq_lane_u64(lanes, 1)) != 0) {
                    for (int i = 0; i < 16; ++i) {
                        const char* candidate = p + i;
                        if (candidate + 1 >= end) {
                            break;
                        }
                        if (candidate[0] == '\r' && candidate[1] == '\n') {
                            return candidate;
                        }
                    }
                }
                p += 16;
            }
            return p;
#else
            return begin;
#endif
        }

        bool addWouldOverflow(size_t lhs, size_t rhs)
        {
            return lhs > std::numeric_limits<size_t>::max() - rhs;
        }
    }

    // RedisReply实现
    RedisReply::RedisReply()
        : m_type(RespType::Null), m_data(std::monostate{})
    {
    }

    RedisReply::RedisReply(RespType type, RespData data)
        : m_type(type), m_data(std::move(data))
    {
    }

    RedisReply::RedisReply(const RedisReply& other)
        : m_type(other.m_type), m_data(other.m_data)
    {
    }

    RedisReply::RedisReply(RedisReply&& other) noexcept
        : m_type(other.m_type), m_data(std::move(other.m_data))
    {
    }

    RedisReply& RedisReply::operator=(const RedisReply& other)
    {
        if (this != &other) {
            m_type = other.m_type;
            m_data = other.m_data;
        }
        return *this;
    }

    RedisReply& RedisReply::operator=(RedisReply&& other) noexcept
    {
        if (this != &other) {
            m_type = other.m_type;
            m_data = std::move(other.m_data);
        }
        return *this;
    }

    std::string RedisReply::asString() const
    {
        if (auto* str = std::get_if<std::string>(&m_data)) {
            return *str;
        }
        return "";
    }

    int64_t RedisReply::asInteger() const
    {
        if (auto* val = std::get_if<int64_t>(&m_data)) {
            return *val;
        }
        return 0;
    }

    double RedisReply::asDouble() const
    {
        if (auto* val = std::get_if<double>(&m_data)) {
            return *val;
        }
        return 0.0;
    }

    bool RedisReply::asBoolean() const
    {
        if (auto* val = std::get_if<bool>(&m_data)) {
            return *val;
        }
        return false;
    }

    const std::vector<RedisReply>& RedisReply::asArray() const
    {
        static std::vector<RedisReply> empty;
        if (auto* arr = std::get_if<std::vector<RedisReply>>(&m_data)) {
            return *arr;
        }
        return empty;
    }

    const std::vector<std::pair<RedisReply, RedisReply>>& RedisReply::asMap() const
    {
        static std::vector<std::pair<RedisReply, RedisReply>> empty;
        if (auto* map = std::get_if<std::vector<std::pair<RedisReply, RedisReply>>>(&m_data)) {
            return *map;
        }
        return empty;
    }

    // RespParser实现
    RespParser::RespParser()
    {
    }

    RespParser::~RespParser()
    {
    }

    void RespParser::reset()
    {
    }

    std::optional<size_t> RespParser::findCRLF(const char* data, size_t length, size_t offset)
    {
        if (offset >= length || length < 2) {
            return std::nullopt;
        }
        const char* begin = data + offset;
        const char* end = data + length;

        // For short messages, scalar scan is cheaper than SIMD setup.
        if (static_cast<size_t>(end - begin) < 64U) {
            for (const char* p = begin; p + 1 < end; ++p) {
                if (p[0] == '\r' && p[1] == '\n') {
                    return static_cast<size_t>(p - data);
                }
            }
            return std::nullopt;
        }

        const char* p = findCRLFSimd(begin, end);
        while (p + 1 < end) {
            const void* found = memchr(p, '\r', static_cast<size_t>(end - p - 1));
            if (!found) return std::nullopt;
            const char* candidate = static_cast<const char*>(found);
            if (candidate[1] == '\n') {
                return static_cast<size_t>(candidate - data);
            }
            p = candidate + 1;
        }
        return std::nullopt;
    }

    std::expected<int64_t, ParseError> RespParser::parseIntegerValue(const char* data, size_t length)
    {
        if (length == 0) {
            return std::unexpected(ParseError::InvalidFormat);
        }
        int64_t result = 0;
        const char* begin = data;
        const char* end = data + length;
        auto [ptr, ec] = std::from_chars(begin, end, result);
        if (ec != std::errc() || ptr != end) {
            return std::unexpected(ParseError::InvalidFormat);
        }
        return result;
    }

    std::expected<std::pair<size_t, RedisReply>, ParseError>
    RespParser::parse(const char* data, size_t length)
    {
        RedisReply reply;
        auto fast_result = parseFast(data, length, &reply);
        if (!fast_result) {
            return std::unexpected(fast_result.error());
        }
        return std::make_pair(fast_result.value(), std::move(reply));
    }

    std::expected<size_t, ParseError>
    RespParser::parseFast(const char* data, size_t length, RedisReply* out)
    {
        if (out == nullptr) {
            return std::unexpected(ParseError::InvalidFormat);
        }
        if (length < 1) {
            return std::unexpected(ParseError::Incomplete);
        }

        char type_marker = data[0];
        switch (type_marker) {
            case '+':  // Simple String
                return parseSimpleStringFast(data, length, out);
            case '-':  // Error
                return parseErrorFast(data, length, out);
            case ':':  // Integer
                return parseIntegerFast(data, length, out);
            case '$':  // Bulk String
                return parseBulkStringFast(data, length, out);
            case '*':  // Array
                return parseArrayFast(data, length, out);
            case ',':  // Double (RESP3)
                return parseDoubleFast(data, length, out);
            case '#':  // Boolean (RESP3)
                return parseBooleanFast(data, length, out);
            case '%':  // Map (RESP3)
                return parseMapFast(data, length, out);
            case '~':  // Set (RESP3)
                return parseSetFast(data, length, out);
            case '>':  // Push (RESP3)
                return parseArrayFast(data, length, out);
            case '=':  // VerbatimString (RESP3)
                return parseBulkStringFast(data, length, out);
            case '(':  // BigNumber (RESP3)
                return parseSimpleStringFast(data, length, out);
            case '!':  // BlobError (RESP3)
                return parseBulkStringFast(data, length, out);
            default:
                return std::unexpected(ParseError::InvalidType);
        }
    }

    std::expected<size_t, ParseError>
    RespParser::parseSimpleStringFast(const char* data, size_t length, RedisReply* out)
    {
        // Hot path for short status replies such as "+OK\r\n".
        if (length >= 5 && data[3] == '\r' && data[4] == '\n') {
            std::string value(data + 1, 2);
            *out = RedisReply(RespType::SimpleString, std::move(value));
            return 5;
        }

        auto crlf_pos = findCRLF(data, length, 1);
        if (!crlf_pos) {
            return std::unexpected(ParseError::Incomplete);
        }

        std::string value(data + 1, *crlf_pos - 1);
        *out = RedisReply(RespType::SimpleString, std::move(value));
        return *crlf_pos + 2;
    }

    std::expected<size_t, ParseError>
    RespParser::parseErrorFast(const char* data, size_t length, RedisReply* out)
    {
        auto crlf_pos = findCRLF(data, length, 1);
        if (!crlf_pos) {
            return std::unexpected(ParseError::Incomplete);
        }

        std::string value(data + 1, *crlf_pos - 1);
        *out = RedisReply(RespType::Error, std::move(value));
        return *crlf_pos + 2;
    }

    std::expected<size_t, ParseError>
    RespParser::parseIntegerFast(const char* data, size_t length, RedisReply* out)
    {
        auto crlf_pos = findCRLF(data, length, 1);
        if (!crlf_pos) {
            return std::unexpected(ParseError::Incomplete);
        }

        auto int_result = parseIntegerValue(data + 1, *crlf_pos - 1);
        if (!int_result) {
            return std::unexpected(int_result.error());
        }

        *out = RedisReply(RespType::Integer, *int_result);
        return *crlf_pos + 2;
    }

    std::expected<size_t, ParseError>
    RespParser::parseBulkStringFast(const char* data, size_t length, RedisReply* out)
    {
        auto crlf_pos = findCRLF(data, length, 1);
        if (!crlf_pos) {
            return std::unexpected(ParseError::Incomplete);
        }

        auto len_result = parseIntegerValue(data + 1, *crlf_pos - 1);
        if (!len_result) {
            return std::unexpected(len_result.error());
        }

        int64_t str_len = *len_result;
        if (str_len == -1) {
            *out = RedisReply(RespType::Null, std::monostate{});
            return *crlf_pos + 2;
        }
        if (str_len < 0) {
            return std::unexpected(ParseError::InvalidLength);
        }
        if (str_len > kMaxRespBulkLength) {
            return std::unexpected(ParseError::InvalidLength);
        }

        size_t content_start = *crlf_pos + 2;
        const size_t str_size = static_cast<size_t>(str_len);
        if (addWouldOverflow(content_start, str_size)) {
            return std::unexpected(ParseError::BufferOverflow);
        }
        size_t content_end = content_start + str_size;
        if (addWouldOverflow(content_end, 2)) {
            return std::unexpected(ParseError::BufferOverflow);
        }
        if (str_size > length - content_start || content_end + 2 > length) {
            return std::unexpected(ParseError::Incomplete);
        }
        if (data[content_end] != '\r' || data[content_end + 1] != '\n') {
            return std::unexpected(ParseError::InvalidFormat);
        }

        std::string value(data + content_start, static_cast<size_t>(str_len));
        *out = RedisReply(RespType::BulkString, std::move(value));
        return content_end + 2;
    }

    std::expected<size_t, ParseError>
    RespParser::parseArrayFast(const char* data, size_t length, RedisReply* out)
    {
        auto crlf_pos = findCRLF(data, length, 1);
        if (!crlf_pos) {
            return std::unexpected(ParseError::Incomplete);
        }

        auto len_result = parseIntegerValue(data + 1, *crlf_pos - 1);
        if (!len_result) {
            return std::unexpected(len_result.error());
        }

        int64_t array_len = *len_result;
        if (array_len == -1) {
            *out = RedisReply(RespType::Null, std::monostate{});
            return *crlf_pos + 2;
        }
        if (array_len < 0) {
            return std::unexpected(ParseError::InvalidLength);
        }
        if (array_len > kMaxRespAggregateLength) {
            return std::unexpected(ParseError::InvalidLength);
        }

        std::vector<RedisReply> elements;
        elements.reserve(static_cast<size_t>(array_len));
        size_t offset = *crlf_pos + 2;

        for (int64_t i = 0; i < array_len; ++i) {
            if (offset >= length) {
                return std::unexpected(ParseError::Incomplete);
            }
            RedisReply element;
            auto elem_consumed = parseFast(data + offset, length - offset, &element);
            if (!elem_consumed) {
                return std::unexpected(elem_consumed.error());
            }
            offset += elem_consumed.value();
            elements.push_back(std::move(element));
        }

        *out = RedisReply(RespType::Array, std::move(elements));
        return offset;
    }

    std::expected<size_t, ParseError>
    RespParser::parseDoubleFast(const char* data, size_t length, RedisReply* out)
    {
        auto crlf_pos = findCRLF(data, length, 1);
        if (!crlf_pos) {
            return std::unexpected(ParseError::Incomplete);
        }
        std::string str(data + 1, *crlf_pos - 1);
        double value;
        try {
            value = std::stod(str);
        } catch (...) {
            return std::unexpected(ParseError::InvalidFormat);
        }

        *out = RedisReply(RespType::Double, value);
        return *crlf_pos + 2;
    }

    std::expected<size_t, ParseError>
    RespParser::parseBooleanFast(const char* data, size_t length, RedisReply* out)
    {
        if (length < 4) {  // #t\r\n or #f\r\n
            return std::unexpected(ParseError::Incomplete);
        }

        bool value;
        if (data[1] == 't') {
            value = true;
        } else if (data[1] == 'f') {
            value = false;
        } else {
            return std::unexpected(ParseError::InvalidFormat);
        }
        if (data[2] != '\r' || data[3] != '\n') {
            return std::unexpected(ParseError::InvalidFormat);
        }

        *out = RedisReply(RespType::Boolean, value);
        return 4;
    }

    std::expected<size_t, ParseError>
    RespParser::parseMapFast(const char* data, size_t length, RedisReply* out)
    {
        auto crlf_pos = findCRLF(data, length, 1);
        if (!crlf_pos) {
            return std::unexpected(ParseError::Incomplete);
        }

        auto len_result = parseIntegerValue(data + 1, *crlf_pos - 1);
        if (!len_result) {
            return std::unexpected(len_result.error());
        }

        int64_t map_size = *len_result;
        if (map_size < 0) {
            return std::unexpected(ParseError::InvalidLength);
        }
        if (map_size > kMaxRespMapPairs) {
            return std::unexpected(ParseError::InvalidLength);
        }

        std::vector<std::pair<RedisReply, RedisReply>> map_data;
        map_data.reserve(static_cast<size_t>(map_size));
        size_t offset = *crlf_pos + 2;

        for (int64_t i = 0; i < map_size; ++i) {
            if (offset >= length) {
                return std::unexpected(ParseError::Incomplete);
            }
            RedisReply key_reply;
            auto key_consumed = parseFast(data + offset, length - offset, &key_reply);
            if (!key_consumed) {
                return std::unexpected(key_consumed.error());
            }
            offset += key_consumed.value();

            if (offset >= length) {
                return std::unexpected(ParseError::Incomplete);
            }
            RedisReply value_reply;
            auto value_consumed = parseFast(data + offset, length - offset, &value_reply);
            if (!value_consumed) {
                return std::unexpected(value_consumed.error());
            }
            offset += value_consumed.value();
            map_data.emplace_back(std::move(key_reply), std::move(value_reply));
        }

        *out = RedisReply(RespType::Map, std::move(map_data));
        return offset;
    }

    std::expected<size_t, ParseError>
    RespParser::parseSetFast(const char* data, size_t length, RedisReply* out)
    {
        auto crlf_pos = findCRLF(data, length, 1);
        if (!crlf_pos) {
            return std::unexpected(ParseError::Incomplete);
        }

        auto len_result = parseIntegerValue(data + 1, *crlf_pos - 1);
        if (!len_result) {
            return std::unexpected(len_result.error());
        }

        int64_t set_size = *len_result;
        if (set_size < 0) {
            return std::unexpected(ParseError::InvalidLength);
        }
        if (set_size > kMaxRespAggregateLength) {
            return std::unexpected(ParseError::InvalidLength);
        }

        std::vector<RedisReply> set_data;
        set_data.reserve(static_cast<size_t>(set_size));
        size_t offset = *crlf_pos + 2;

        for (int64_t i = 0; i < set_size; ++i) {
            if (offset >= length) {
                return std::unexpected(ParseError::Incomplete);
            }
            RedisReply element;
            auto elem_consumed = parseFast(data + offset, length - offset, &element);
            if (!elem_consumed) {
                return std::unexpected(elem_consumed.error());
            }
            offset += elem_consumed.value();
            set_data.push_back(std::move(element));
        }

        *out = RedisReply(RespType::Set, std::move(set_data));
        return offset;
    }

    // RespEncoder实现
    RespEncoder::RespEncoder()
    {
    }

    RespEncoder::~RespEncoder()
    {
    }

    std::string RespEncoder::encodeSimpleString(const std::string& str)
    {
        return "+" + str + "\r\n";
    }

    std::string RespEncoder::encodeError(const std::string& error)
    {
        return "-" + error + "\r\n";
    }

    std::string RespEncoder::encodeInteger(int64_t value)
    {
        return ":" + std::to_string(value) + "\r\n";
    }

    std::string RespEncoder::encodeBulkString(const std::string& str)
    {
        std::string result;
        result.reserve(estimateBulkStringBytes(str.size()));
        appendBulkString(result, str);
        return result;
    }

    std::string RespEncoder::encodeNull()
    {
        return "$-1\r\n";
    }

    std::string RespEncoder::encodeArray(const std::vector<std::string>& elements)
    {
        size_t estimated = 1 + decimalDigits(elements.size()) + 2;
        for (const auto& elem : elements) {
            estimated += estimateBulkStringBytes(elem.size());
        }

        std::string result;
        result.reserve(estimated);
        result.push_back('*');
        appendUnsignedDecimal(result, elements.size());
        result += "\r\n";
        for (const auto& elem : elements) {
            appendBulkString(result, elem);
        }
        return result;
    }

    std::string RespEncoder::encodeCommand(std::initializer_list<std::string> cmd_parts)
    {
        if (cmd_parts.size() == 0) {
            return "*0\r\n";
        }

        size_t estimated = 1 + decimalDigits(cmd_parts.size()) + 2;
        for (const auto& part : cmd_parts) {
            estimated += estimateBulkStringBytes(part.size());
        }

        std::string result;
        result.reserve(estimated);
        result.push_back('*');
        appendUnsignedDecimal(result, cmd_parts.size());
        result += "\r\n";
        for (const auto& part : cmd_parts) {
            appendBulkString(result, part);
        }
        return result;
    }

    void RespEncoder::append(std::string& out, const std::vector<std::string>& cmd_parts) const
    {
        if (cmd_parts.empty()) {
            out += "*0\r\n";
            return;
        }

        size_t estimated = out.size() + 1 + decimalDigits(cmd_parts.size()) + 2;
        for (const auto& part : cmd_parts) {
            estimated += estimateBulkStringBytes(part.size());
        }
        out.reserve(estimated);

        out.push_back('*');
        appendUnsignedDecimal(out, cmd_parts.size());
        out += "\r\n";
        for (const auto& part : cmd_parts) {
            appendBulkString(out, part);
        }
    }

    void RespEncoder::append(std::string& out,
                                    std::string_view cmd,
                                    const std::vector<std::string>& args) const
    {
        const size_t arg_count = 1 + args.size();
        size_t estimated = out.size() + 1 + decimalDigits(arg_count) + 2 + estimateBulkStringBytes(cmd.size());
        for (const auto& arg : args) {
            estimated += estimateBulkStringBytes(arg.size());
        }
        out.reserve(estimated);

        out.push_back('*');
        appendUnsignedDecimal(out, arg_count);
        out += "\r\n";
        appendBulkString(out, cmd);
        for (const auto& arg : args) {
            appendBulkString(out, arg);
        }
    }

    void RespEncoder::append(std::string& out,
                                    std::string_view cmd,
                                    std::span<const std::string_view> args) const
    {
        const size_t arg_count = 1 + args.size();
        size_t estimated = out.size() + 1 + decimalDigits(arg_count) + 2 + estimateBulkStringBytes(cmd.size());
        for (const auto& arg : args) {
            estimated += estimateBulkStringBytes(arg.size());
        }
        out.reserve(estimated);

        out.push_back('*');
        appendUnsignedDecimal(out, arg_count);
        out += "\r\n";
        appendBulkString(out, cmd);
        for (const auto& arg : args) {
            appendBulkString(out, arg);
        }
    }

    void RespEncoder::append(std::string& out,
                                    std::string_view cmd,
                                    std::initializer_list<std::string_view> args) const
    {
        const size_t arg_count = 1 + args.size();
        size_t estimated = out.size() + 1 + decimalDigits(arg_count) + 2 + estimateBulkStringBytes(cmd.size());
        for (const auto& arg : args) {
            estimated += estimateBulkStringBytes(arg.size());
        }
        out.reserve(estimated);

        out.push_back('*');
        appendUnsignedDecimal(out, arg_count);
        out += "\r\n";
        appendBulkString(out, cmd);
        for (const auto& arg : args) {
            appendBulkString(out, arg);
        }
    }

    std::string RespEncoder::encodeCommand(const std::string& cmd, std::initializer_list<std::string> args)
    {
        const size_t arg_count = 1 + args.size();
        size_t estimated = 1 + decimalDigits(arg_count) + 2 + estimateBulkStringBytes(cmd.size());
        for (const auto& arg : args) {
            estimated += estimateBulkStringBytes(arg.size());
        }

        std::string result;
        result.reserve(estimated);
        result.push_back('*');
        appendUnsignedDecimal(result, arg_count);
        result += "\r\n";
        appendBulkString(result, cmd);
        for (const auto& arg : args) {
            appendBulkString(result, arg);
        }
        return result;
    }

    std::string RespEncoder::encodeDouble(double value)
    {
        return "," + std::to_string(value) + "\r\n";
    }

    std::string RespEncoder::encodeBoolean(bool value)
    {
        return value ? "#t\r\n" : "#f\r\n";
    }

} // namespace galay::redis::protocol
