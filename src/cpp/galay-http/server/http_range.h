/**
 * @file http_range.h
 * @brief HTTP Range 请求解析与处理
 * @author galay-http
 * @version 1.0.0
 *
 * @details 提供 HTTP Range 请求头的解析功能，支持单范围、多范围、
 * 后缀范围和前缀范围四种格式。可生成 Content-Range 响应头，
 * 并配合 ETag 实现 If-Range 条件请求。
 */

#ifndef GALAY_HTTP_RANGE_H
#define GALAY_HTTP_RANGE_H

#include <algorithm>
#include <charconv>
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>
#include <ctime>
#include "http_etag.h"

namespace galay::http
{

/**
 * @brief HTTP Range 请求结构
 * @details 表示一个 HTTP Range 请求的范围
 */
struct HttpRange
{
    uint64_t start;     ///< 起始字节位置
    uint64_t end;       ///< 结束字节位置
    uint64_t length;    ///< 范围长度

    /**
     * @brief 构造函数
     * @param s 起始位置
     * @param e 结束位置
     */
    HttpRange(uint64_t s, uint64_t e)
        : start(s), end(e), length(e - s + 1)
    {
    }

    /**
     * @brief 默认构造函数
     */
    HttpRange()
        : start(0), end(0), length(0)
    {
    }

    /**
     * @brief 检查范围是否有效
     * @return 是否有效
     */
    bool isValid() const
    {
        return start <= end && length > 0;
    }
};

/**
 * @brief HTTP Range 请求类型
 */
enum class RangeType
{
    /**
     * @brief 单个范围
     * @example Range: bytes=0-499
     */
    SINGLE_RANGE,

    /**
     * @brief 多个范围（multipart）
     * @example Range: bytes=0-499,1000-1499
     */
    MULTIPLE_RANGES,

    /**
     * @brief 后缀范围（从指定位置到文件末尾）
     * @example Range: bytes=500-
     */
    SUFFIX_RANGE,

    /**
     * @brief 前缀范围（最后 N 个字节）
     * @example Range: bytes=-500
     */
    PREFIX_RANGE,

    /**
     * @brief 无效的 Range 请求
     */
    INVALID
};

/**
 * @brief HTTP Range 解析结果
 */
struct RangeParseResult
{
    std::vector<HttpRange> ranges;        ///< Range 列表
    std::string boundary;                 ///< multipart 边界（多范围时使用）
    RangeType type;                       ///< Range 类型

    /**
     * @brief 默认构造函数
     */
    RangeParseResult()
        : type(RangeType::INVALID)
    {
    }

    /**
     * @brief 构造函数
     * @param t Range 类型
     * @param r Range 列表
     */
    RangeParseResult(RangeType t, const std::vector<HttpRange>& r)
        : ranges(r)
        , type(t)
    {
    }

    /**
     * @brief 检查是否有效
     * @return 是否有效
     */
    bool isValid() const
    {
        return type != RangeType::INVALID && !ranges.empty();
    }

    /**
     * @brief 生成随机边界（用于 multipart 响应）
     * @return 边界字符串
     */
    static std::string generateBoundary()
    {
        static uint64_t counter = 0;
        return "multipart_boundary_" + std::to_string(counter++) + "_" +
               std::to_string(std::time(nullptr));
    }
};

/**
 * @brief HTTP Range 解析器
 * @details 解析 HTTP Range 请求头，支持多种 Range 格式
 *
 * 支持的格式：
 * - Range: bytes=0-499              (单范围)
 * - Range: bytes=0-499,1000-1499     (多范围)
 * - Range: bytes=500-               (后缀范围)
 * - Range: bytes=-500               (前缀范围，最后500字节)
 */
class HttpRangeParser
{
public:
    static constexpr size_t kMaxMultipartRanges = 16;                  ///< multipart Range 最大分片数
    static constexpr uint64_t kMaxMultipartBodyBytes = 8ull * 1024 * 1024; ///< multipart Range 最大聚合内容字节数

    /**
     * @brief 解析 Range 请求头
     * @param rangeHeader Range 请求头的值（不包含 "Range: " 前缀）
     * @param fileSize 文件总大小（用于验证范围）
     * @return Range 解析结果
     */
    static RangeParseResult parse(const std::string& rangeHeader, uint64_t fileSize)
    {
        RangeParseResult result;

        // 检查是否为 bytes 单位
        const std::string bytesPrefix = "bytes=";
        if (rangeHeader.empty() || rangeHeader.substr(0, bytesPrefix.size()) != bytesPrefix) {
            return result;  // 无效的 Range
        }

        // 提取范围部分
        std::string rangesStr = rangeHeader.substr(bytesPrefix.size());
        if (rangesStr.empty()) {
            return result;  // 无效的 Range
        }

        // 解析范围
        auto ranges = splitRanges(rangesStr);
        if (ranges.empty() || ranges.size() > kMaxMultipartRanges) {
            return result;  // 无效的 Range
        }

        // 判断 Range 类型
        if (ranges.size() == 1) {
            // 单个范围
            auto range = parseSingleRange(ranges[0], fileSize);
            if (range.isValid()) {
                result.type = RangeType::SINGLE_RANGE;
                result.ranges.push_back(range);
            }
        } else {
            // 多个范围
            std::vector<HttpRange> parsed_ranges;
            parsed_ranges.reserve(ranges.size());
            for (const auto& r : ranges) {
                auto range = parseSingleRange(r, fileSize);
                if (range.isValid()) {
                    parsed_ranges.push_back(range);
                }
            }

            // 如果所有范围都无效
            if (parsed_ranges.empty()) {
                return result;
            }

            result.ranges = mergeRanges(std::move(parsed_ranges));
            if (result.ranges.empty() || !isAggregateRangeSizeAllowed(result.ranges)) {
                result.type = RangeType::INVALID;
                result.ranges.clear();
                return result;
            }

            result.type = result.ranges.size() == 1
                ? RangeType::SINGLE_RANGE
                : RangeType::MULTIPLE_RANGES;
            if (result.type == RangeType::MULTIPLE_RANGES) {
                result.boundary = RangeParseResult::generateBoundary();
            }
        }

        return result;
    }

    /**
     * @brief 生成 Content-Range 响应头值
     * @param start 起始位置
     * @param end 结束位置
     * @param fileSize 文件总大小
     * @return Content-Range 头的值
     */
    static std::string makeContentRange(uint64_t start, uint64_t end, uint64_t fileSize)
    {
        return "bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" +
               std::to_string(fileSize);
    }

    /**
     * @brief 生成 Content-Range 响应头值（使用 HttpRange）
     * @param range Range 对象
     * @param fileSize 文件总大小
     * @return Content-Range 头的值
     */
    static std::string makeContentRange(const HttpRange& range, uint64_t fileSize)
    {
        return makeContentRange(range.start, range.end, fileSize);
    }

    /**
     * @brief 检查是否满足 If-Range 条件
     * @param ifRangeHeader If-Range 请求头的值
     * @param etag 文件的 ETag
     * @param lastModified 文件的最后修改时间
     * @return 是否满足条件
     */
    static bool checkIfRange(const std::string& ifRangeHeader,
                           const std::string& etag,
                           std::time_t lastModified)
    {
        return ETagGenerator::matchIfRange(etag, ifRangeHeader, lastModified);
    }

private:
    static bool parseUnsigned(std::string_view text, uint64_t& value)
    {
        if (text.empty()) {
            return false;
        }

        const char* begin = text.data();
        const char* end = begin + text.size();
        auto [ptr, ec] = std::from_chars(begin, end, value);
        return ec == std::errc{} && ptr == end;
    }

    static std::vector<HttpRange> mergeRanges(std::vector<HttpRange> ranges)
    {
        std::sort(ranges.begin(), ranges.end(), [](const HttpRange& lhs, const HttpRange& rhs) {
            if (lhs.start != rhs.start) {
                return lhs.start < rhs.start;
            }
            return lhs.end < rhs.end;
        });

        std::vector<HttpRange> merged;
        merged.reserve(ranges.size());
        for (const auto& range : ranges) {
            if (merged.empty()) {
                merged.push_back(range);
                continue;
            }

            auto& tail = merged.back();
            if (range.start <= tail.end ||
                (tail.end != UINT64_MAX && range.start == tail.end + 1)) {
                tail.end = std::max(tail.end, range.end);
                tail.length = tail.end - tail.start + 1;
                continue;
            }
            merged.push_back(range);
        }
        return merged;
    }

    static bool isAggregateRangeSizeAllowed(const std::vector<HttpRange>& ranges)
    {
        uint64_t total = 0;
        for (const auto& range : ranges) {
            if (range.length > kMaxMultipartBodyBytes ||
                total > kMaxMultipartBodyBytes - range.length) {
                return false;
            }
            total += range.length;
        }
        return true;
    }

    /**
     * @brief 分割多个 Range
     * @param rangesStr Range 字符串
     * @return Range 列表
     */
    static std::vector<std::string> splitRanges(const std::string& rangesStr)
    {
        std::vector<std::string> ranges;
        std::string current;
        bool inRange = false;

        for (char c : rangesStr) {
            if (c == ',') {
                if (inRange) {
                    ranges.push_back(current);
                    current.clear();
                    inRange = false;
                }
            } else if (!std::isspace(static_cast<unsigned char>(c))) {
                current += c;
                inRange = true;
            }
        }

        if (inRange && !current.empty()) {
            ranges.push_back(current);
        }

        return ranges;
    }

    /**
     * @brief 解析单个 Range
     * @param rangeStr Range 字符串
     * @param fileSize 文件总大小
     * @return HttpRange 对象
     */
    static HttpRange parseSingleRange(const std::string& rangeStr, uint64_t fileSize)
    {
        if (fileSize == 0) {
            return HttpRange();
        }

        size_t dashPos = rangeStr.find('-');
        if (dashPos == std::string::npos) {
            return HttpRange();  // 无效格式
        }

        std::string_view startStr(rangeStr.data(), dashPos);
        std::string_view endStr(rangeStr.data() + dashPos + 1, rangeStr.size() - dashPos - 1);

        // 情况1: bytes=start-end
        if (!startStr.empty() && !endStr.empty()) {
            uint64_t start = 0;
            uint64_t end = 0;
            if (!parseUnsigned(startStr, start) || !parseUnsigned(endStr, end)) {
                return HttpRange();
            }

            // 验证范围
            if (start > end || start >= fileSize) {
                return HttpRange();  // 无效范围
            }

            // 限制 end 不能超过文件大小
            if (end >= fileSize) {
                end = fileSize - 1;
            }

            return HttpRange(start, end);
        }

        // 情况2: bytes=start- (后缀范围)
        if (!startStr.empty() && endStr.empty()) {
            uint64_t start = 0;
            if (!parseUnsigned(startStr, start)) {
                return HttpRange();
            }

            if (start >= fileSize) {
                return HttpRange();  // 无效范围
            }

            return HttpRange(start, fileSize - 1);
        }

        // 情况3: bytes=-suffix (前缀范围，最后 N 个字节)
        if (startStr.empty() && !endStr.empty()) {
            uint64_t suffix = 0;
            if (!parseUnsigned(endStr, suffix)) {
                return HttpRange();
            }

            if (suffix == 0 || suffix > fileSize) {
                suffix = fileSize;
            }

            uint64_t start = fileSize - suffix;
            return HttpRange(start, fileSize - 1);
        }

        return HttpRange();  // 无效格式
    }
};

} // namespace galay::http

#endif // GALAY_HTTP_RANGE_H
