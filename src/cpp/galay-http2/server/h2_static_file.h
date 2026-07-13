/**
 * @file h2_static_file.h
 * @brief HTTP/2 static file metadata and small-file cache
 */

#ifndef GALAY_HTTP2_SERVER_H2_STATIC_FILE_H
#define GALAY_HTTP2_SERVER_H2_STATIC_FILE_H

#include "../protoc/http2_hpack.h"

#include <atomic>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace galay::http2
{

struct H2StaticFileConfig {
    std::filesystem::path root;
    size_t small_file_threshold = 64 * 1024;
    bool enable_etag = true;
};

struct H2StaticFileRequest {
    std::string path;
    std::string if_none_match;
    std::string range;
};

/**
 * @brief HTTP/2 静态文件小文件 body 的异步发布槽。
 *
 * @details cache 元数据只由连接 IO owner 同步访问；body 由 blocking worker 读完后
 *          通过原子 shared_ptr 发布，后续连接可无锁复用同一份小文件内容。
 */
class H2StaticFileBodyCacheSlot {
public:
    H2StaticFileBodyCacheSlot() = default;
    H2StaticFileBodyCacheSlot(const H2StaticFileBodyCacheSlot&) = delete;
    H2StaticFileBodyCacheSlot& operator=(const H2StaticFileBodyCacheSlot&) = delete;
    H2StaticFileBodyCacheSlot(H2StaticFileBodyCacheSlot&&) = delete;
    H2StaticFileBodyCacheSlot& operator=(H2StaticFileBodyCacheSlot&&) = delete;

    std::shared_ptr<const std::string> load() const noexcept {
        return std::atomic_load_explicit(&m_body, std::memory_order_acquire);
    }

    bool storeIfEmpty(std::shared_ptr<const std::string> body) noexcept {
        if (!body) {
            return false;
        }
        std::shared_ptr<const std::string> expected;
        return std::atomic_compare_exchange_strong_explicit(
            &m_body,
            &expected,
            std::move(body),
            std::memory_order_acq_rel,
            std::memory_order_acquire);
    }

private:
    std::shared_ptr<const std::string> m_body;  ///< 通过 shared_ptr 原子自由函数发布和读取
};

struct H2StaticFileLookup {
    std::filesystem::path file_path;
    std::string etag;
    std::string content_type = "application/octet-stream";
    std::shared_ptr<const std::string> body;
    std::shared_ptr<const std::string> encoded_headers;
    std::shared_ptr<H2StaticFileBodyCacheSlot> body_cache_slot;
    std::vector<Http2HeaderField> headers;
    uintmax_t file_size = 0;
    std::time_t last_modified = 0;
    uintmax_t range_start = 0;
    uintmax_t range_end = 0;
    int status = 404;
    bool body_cached = false;
    bool body_cacheable = false;
};

struct H2StaticFileFastLookup {
    std::filesystem::path file_path;
    std::shared_ptr<const std::string> body;
    std::shared_ptr<const std::string> encoded_headers;
    std::shared_ptr<H2StaticFileBodyCacheSlot> body_cache_slot;
    uintmax_t content_length = 0;
    bool body_cached = false;
    bool body_cacheable = false;
};

class H2StaticFileCache;

struct H2StaticFileMount {
    std::string prefix;
    H2StaticFileConfig config;
    std::shared_ptr<H2StaticFileCache> cache;
};

H2StaticFileMount makeH2StaticFileMount(std::string prefix, H2StaticFileConfig config);

/**
 * @brief Encode an HTTP/2 static file response header block.
 * @param status HTTP response status code.
 * @param headers Already materialized response headers, without `:status`.
 * @return Shared HPACK header block suitable for reuse by static file send paths.
 * @note The encoder is stateless/no-index, so the returned block is connection-independent.
 */
std::shared_ptr<const std::string> encodeH2StaticFileHeaders(
    int status,
    const std::vector<Http2HeaderField>& headers);

class H2StaticFileCache {
public:
    explicit H2StaticFileCache(H2StaticFileConfig config);

    H2StaticFileLookup lookup(const H2StaticFileRequest& request);
    std::optional<H2StaticFileFastLookup> lookupFast200(std::string_view request_path);

private:
    struct Entry {
        std::filesystem::path file_path;
        std::string etag;
        std::string content_type;
        std::shared_ptr<const std::string> body;
        std::shared_ptr<const std::string> encoded_headers;
        std::shared_ptr<H2StaticFileBodyCacheSlot> body_cache_slot;
        std::vector<Http2HeaderField> headers;
        uintmax_t file_size = 0;
        std::time_t last_modified = 0;
        bool body_cached = false;
        bool body_cacheable = false;
    };

    // 返回指向内部缓存的临时视图；调用方必须在当前同步调用栈内消费，不能保存。
    Entry* findOrLoadEntry(std::string_view request_path);
    std::filesystem::path normalizeRequestPath(const std::string& request_path) const;
    bool isInsideRoot(const std::filesystem::path& path) const;
    H2StaticFileLookup makeNotFound() const;
    H2StaticFileLookup makeLookup(const Entry& entry, int status) const;
    Entry loadEntry(const std::filesystem::path& file_path) const;

    H2StaticFileConfig m_config;
    std::filesystem::path m_root;
    std::unordered_map<std::string, std::string> m_request_path_cache;
    std::unordered_map<std::string, Entry> m_cache;
};

} // namespace galay::http2

#endif // GALAY_HTTP2_SERVER_H2_STATIC_FILE_H
