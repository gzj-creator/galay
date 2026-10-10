/**
 * @file http_etag.h
 * @brief HTTP ETag 生成与匹配
 * @author galay-http
 * @version 1.0.0
 *
 * @details 提供 HTTP ETag 的生成、比较和匹配功能，
 * 支持强 ETag 和弱 ETag 两种模式。
 * 用于 HTTP 条件请求（If-None-Match、If-Match、If-Range）的处理。
 */

#ifndef GALAY_HTTP_ETAG_H
#define GALAY_HTTP_ETAG_H

#include <string>
#include <ctime>
#include <time.h>
#include <cstdint>
#include <vector>
#include <sstream>
#include <iomanip>
#include <locale>
#include <filesystem>
#include <sys/stat.h>

namespace galay::http
{

namespace fs = std::filesystem;

/**
 * @brief ETag 生成器
 * @details 为文件生成 ETag，支持强 ETag 和弱 ETag
 *
 * 生产级实现：
 * - 使用文件的真实 inode、大小和修改时间
 * - 强 ETag 基于文件内容的唯一性（inode + mtime + size）
 * - 弱 ETag 仅基于修改时间和大小
 */
class ETagGenerator
{
public:
    /**
     * @brief ETag 类型
     */
    enum class Type
    {
        STRONG,  ///< 强 ETag - 基于 inode + mtime + size
        WEAK     ///< 弱 ETag - 仅基于 mtime + size
    };

    /**
     * @brief 生成文件的强 ETag
     * @param filePath 文件路径
     * @param fileSize 文件字节数
     * @param lastModified 文件最后修改时间
     * @return 带引号的强实体标签
     * @details 使用 inode + mtime + size 生成，确保文件唯一性
     */
    static std::string generate_strong(const fs::path& filePath, size_t fileSize, std::time_t lastModified)
    {
        uint64_t inode = get_file_inode(filePath);
        char etag[128];
        snprintf(etag, sizeof(etag), "\"%lx-%zx-%lx\"",
                 static_cast<unsigned long>(inode),
                 fileSize,
                 static_cast<unsigned long>(lastModified));
        return std::string(etag);
    }

    /**
     * @brief 生成文件的弱 ETag
     * @param filePath 文件路径
     * @param fileSize 文件字节数
     * @param lastModified 文件最后修改时间
     * @return 以 W/ 开头的弱实体标签
     * @details 仅使用 mtime + size，适用于内容可能略有差异但语义相同的情况
     */
    static std::string generate_weak(const fs::path& filePath, size_t fileSize, std::time_t lastModified)
    {
        return "W/" + generate_strong(filePath, fileSize, lastModified);
    }

    /**
     * @brief 生成 ETag（自动选择类型）
     * @param filePath 文件路径
     * @param type ETag 类型（强/弱）
     * @return ETag 字符串
     */
    static std::string generate(const fs::path& filePath, Type type = Type::STRONG)
    {
        std::error_code ec;

        // 获取文件大小
        auto fileSize = fs::file_size(filePath, ec);
        if (ec) return "\"\"";

        // 获取文件的真实修改时间
        std::time_t lastModifiedTimeT = get_file_modification_time(filePath);
        if (lastModifiedTimeT == 0) return "\"\"";

        if (type == Type::WEAK) {
            return generate_weak(filePath, fileSize, lastModifiedTimeT);
        } else {
            return generate_strong(filePath, fileSize, lastModifiedTimeT);
        }
    }

    /**
     * @brief 检查 ETag 是否匹配
     * @param etag1 第一个实体标签
     * @param etag2 第二个实体标签
     * @return 两个实体标签规范化后相同时返回 true，否则返回 false
     * @details 支持强弱 ETag 的比较（规范化后比较）
     */
    static bool match(const std::string& etag1, const std::string& etag2)
    {
        return normalize_etag_value(etag1) == normalize_etag_value(etag2);
    }

    /**
     * @brief 检查 If-None-Match 是否匹配当前 ETag
     * @param etag 实体标签
     * @param header_value HTTP 条件请求头的值
     * @return 请求头中的实体标签匹配当前 ETag 时返回 true，否则返回 false
     */
    static bool match_if_none_match(const std::string& etag, const std::string& header_value)
    {
        return match_etag_header(etag, header_value);
    }

    /**
     * @brief 检查 If-Match 是否匹配当前 ETag
     * @param etag 实体标签
     * @param header_value HTTP 条件请求头的值
     * @return 请求头中的实体标签匹配当前 ETag 时返回 true，否则返回 false
     */
    static bool match_if_match(const std::string& etag, const std::string& header_value)
    {
        return match_etag_header(etag, header_value);
    }

    /**
     * @brief 检查 If-Range 是否匹配当前 ETag（日期将与 lastModified 比较）
     * @param etag 实体标签
     * @param header_value HTTP 条件请求头的值
     * @param lastModified 文件最后修改时间
     * @return 条件为空、实体标签匹配或文件未晚于指定日期修改时返回 true，否则返回 false
     */
    static bool match_if_range(const std::string& etag, const std::string& header_value, std::time_t lastModified)
    {
        const std::string headerTrim = trim(header_value);
        if (headerTrim.empty()) {
            return true;
        }

        const bool looksLikeEtag = (headerTrim.front() == '"') ||
            (headerTrim.rfind("W/", 0) == 0);

        if (looksLikeEtag) {
            return normalize_etag_value(headerTrim) == normalize_etag_value(etag);
        }

        std::time_t parsed = 0;
        if (parse_http_date(headerTrim, parsed)) {
            return lastModified <= parsed;
        }

        return false;
    }

    /**
     * @brief 解析 If-None-Match 或 If-Match 头
     * @param header_value HTTP 条件请求头的值
     * @return 解析出的实体标签列表
     * @details 提取所有 ETag 值（不包含引号）
     */
    static std::vector<std::string> parse_if_match(const std::string& header_value)
    {
        std::vector<std::string> etags;
        std::string current;
        bool inEtag = false;

        for (size_t i = 0; i < header_value.size(); ++i) {
            char c = header_value[i];
            if (c == '"') {
                if (inEtag) {
                    if (!current.empty()) {
                        etags.push_back(current);
                        current.clear();
                    }
                    inEtag = false;
                } else {
                    inEtag = true;
                }
            } else if (inEtag) {
                current += c;
            }
        }
        return etags;
    }

    /**
     * @brief 检查是否匹配任何一个 ETag
     * @param etag 实体标签
     * @param etags 实体标签列表
     * @return 列表中存在匹配的实体标签时返回 true，否则返回 false
     */
    static bool match_any(const std::string& etag, const std::vector<std::string>& etags)
    {
        for (const auto& e : etags) {
            if (match(etag, e)) return true;
        }
        return false;
    }

    /**
     * @brief 格式化 HTTP 日期
     * @param time 时间
     * @return RFC 7231 格式的 GMT 日期字符串
     * @details 按照 RFC 7231 格式化为 GMT 时间
     */
    static std::string format_http_date(std::time_t time)
    {
        char buffer[128];
        std::tm tm;
#ifdef _WIN32
        gmtime_s(&tm, &time);
#else
        gmtime_r(&time, &tm);
#endif
        strftime(buffer, sizeof(buffer), "%a, %d %b %Y %H:%M:%S GMT", &tm);
        return std::string(buffer);
    }

private:
    static bool parse_with_format(const std::string& value, const char* format, std::tm& tm, bool twoDigitYear)
    {
        std::istringstream ss(value);
        ss.imbue(std::locale::classic());
        ss >> std::get_time(&tm, format);
        if (ss.fail()) {
            return false;
        }
        ss >> std::ws;
        if (!ss.eof()) {
            return false;
        }
        if (twoDigitYear && tm.tm_year < 70) {
            tm.tm_year += 100;
        }
        return true;
    }

    static std::time_t timegm_utc(std::tm* tm)
    {
    #ifdef _WIN32
        return _mkgmtime(tm);
    #else
        return timegm(tm);
    #endif
    }

    static bool parse_http_date(const std::string& value, std::time_t& out)
    {
        std::tm tm{};
        if (parse_with_format(value, "%a, %d %b %Y %H:%M:%S GMT", tm, false) ||
            parse_with_format(value, "%A, %d-%b-%y %H:%M:%S GMT", tm, true) ||
            parse_with_format(value, "%a %b %e %H:%M:%S %Y", tm, false)) {
            const auto t = timegm_utc(&tm);
            if (t == static_cast<std::time_t>(-1)) {
                return false;
            }
            out = t;
            return true;
        }
        return false;
    }

    static bool match_etag_header(const std::string& etag, const std::string& header_value)
    {
        if (header_value.empty()) {
            return false;
        }

        const std::string headerTrim = trim(header_value);
        if (headerTrim == "*") {
            return true;
        }

        const std::string normalized = normalize_etag_value(etag);

        auto etags = parse_if_match(header_value);
        if (!etags.empty()) {
            for (const auto& e : etags) {
                if (normalize_etag_value(e) == normalized) {
                    return true;
                }
            }
            return false;
        }

        size_t start = 0;
        while (start < header_value.size()) {
            size_t end = header_value.find(',', start);
            std::string token = (end == std::string::npos)
                ? header_value.substr(start)
                : header_value.substr(start, end - start);
            token = trim(std::move(token));
            if (!token.empty()) {
                if (token == "*") {
                    return true;
                }
                if (normalize_etag_value(token) == normalized) {
                    return true;
                }
            }
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
        return false;
    }

    static std::string trim(std::string s)
    {
        const char* ws = " \t\r\n";
        const auto start = s.find_first_not_of(ws);
        if (start == std::string::npos) {
            return {};
        }
        const auto end = s.find_last_not_of(ws);
        return s.substr(start, end - start + 1);
    }

    static std::string normalize_etag_value(std::string etag)
    {
        etag = trim(std::move(etag));
        if (etag.rfind("W/", 0) == 0) {
            etag = etag.substr(2);
        }
        etag = trim(std::move(etag));
        if (etag.size() >= 2 && etag.front() == '"' && etag.back() == '"') {
            etag = etag.substr(1, etag.size() - 2);
        }
        return etag;
    }

    /**
     * @brief 获取文件的真实 inode
     * @param filePath 文件路径
     * @return 文件 inode；查询失败时为 0，Windows 上使用路径哈希
     * @details 使用 stat 系统调用获取文件的 inode 号
     */
    static uint64_t get_file_inode(const fs::path& filePath)
    {
#ifdef _WIN32
        // Windows 没有 inode 概念，使用文件路径哈希作为替代
        return std::hash<std::string>{}(filePath.string());
#else
        struct stat fileStat;
        if (stat(filePath.c_str(), &fileStat) == 0) {
            return static_cast<uint64_t>(fileStat.st_ino);
        }
        // 失败时使用路径哈希作为后备
        return std::hash<std::string>{}(filePath.string());
#endif
    }

    /**
     * @brief 获取文件的真实修改时间
     * @param filePath 文件路径
     * @return 文件修改时间；查询失败时为 0
     * @details 使用 stat 系统调用获取文件的 mtime
     */
    static std::time_t get_file_modification_time(const fs::path& filePath)
    {
#ifdef _WIN32
        // Windows 使用 filesystem 库
        std::error_code ec;
        auto ftime = fs::last_write_time(filePath, ec);
        if (ec) return 0;

        // 转换为 time_t
        auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
            ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now()
        );
        return std::chrono::system_clock::to_time_t(sctp);
#else
        struct stat fileStat;
        if (stat(filePath.c_str(), &fileStat) == 0) {
            return fileStat.st_mtime;
        }
        return 0;
#endif
    }
};

} // namespace galay::http

#endif // GALAY_HTTP_ETAG_H
