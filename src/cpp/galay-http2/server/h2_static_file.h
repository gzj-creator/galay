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
 *          通过标量原子状态一次性发布，后续连接可安全复用同一份小文件内容。
 * @note load() 返回拥有该快照的 shared_ptr，便于异步发送队列跨线程持有 body 生命周期。
 */
class H2StaticFileBodyCacheSlot {
public:
    H2StaticFileBodyCacheSlot() = default;
    H2StaticFileBodyCacheSlot(const H2StaticFileBodyCacheSlot&) = delete;
    H2StaticFileBodyCacheSlot& operator=(const H2StaticFileBodyCacheSlot&) = delete;
    H2StaticFileBodyCacheSlot(H2StaticFileBodyCacheSlot&&) = delete;
    H2StaticFileBodyCacheSlot& operator=(H2StaticFileBodyCacheSlot&&) = delete;

    std::shared_ptr<const std::string> load() const noexcept {
        if (m_state.load(std::memory_order_acquire) != State::kReady) {
            return {};
        }
        return m_body;
    }

    bool store_if_empty(std::shared_ptr<const std::string> body) noexcept {
        if (!body) {
            return false;
        }
        auto expected = State::kEmpty;
        if (!m_state.compare_exchange_strong(expected, State::kPublishing,
                                             std::memory_order_relaxed,
                                             std::memory_order_relaxed)) {
            return false;
        }
        m_body = std::move(body);
        m_state.store(State::kReady, std::memory_order_release);
        return true;
    }

private:
    enum class State : uint8_t { kEmpty, kPublishing, kReady };

    // The winning writer publishes once. Readers never wait for publication,
    // and m_body is immutable after the release/acquire handoff.
    std::atomic<State> m_state{State::kEmpty};
    std::shared_ptr<const std::string> m_body;
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

H2StaticFileMount make_h2_static_file_mount(std::string prefix, H2StaticFileConfig config);

/**
 * @brief encode an HTTP/2 static file response header block.
 * @param status HTTP response status code.
 * @param headers Already materialized response headers, without `:status`.
 * @return Shared HPACK header block suitable for reuse by static file send paths.
 * @note The encoder is stateless/no-index, so the returned block is connection-independent.
 */
std::shared_ptr<const std::string> encode_h2_static_file_headers(
    int status,
    const std::vector<Http2HeaderField>& headers);

/**
 * @brief HTTP/2 static-file metadata and body cache.
 * @details Entries are populated once and are not automatically invalidated when
 *          files on disk change; recreate the mount/cache to observe an update.
 */
class H2StaticFileCache {
public:
    explicit H2StaticFileCache(H2StaticFileConfig config);

    H2StaticFileLookup lookup(const H2StaticFileRequest& request);
    std::optional<H2StaticFileFastLookup> lookup_fast200(std::string_view request_path);

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
    Entry* find_or_load_entry(std::string_view request_path);
    std::filesystem::path normalize_request_path(const std::string& request_path) const;
    bool is_inside_root(const std::filesystem::path& path) const;
    H2StaticFileLookup make_not_found() const;
    H2StaticFileLookup make_lookup(const Entry& entry, int status) const;
    Entry load_entry(const std::filesystem::path& file_path) const;

    H2StaticFileConfig m_config;
    std::filesystem::path m_root;
    std::unordered_map<std::string, std::string> m_request_path_cache;
    std::unordered_map<std::string, Entry> m_cache;
};

} // namespace galay::http2

#endif // GALAY_HTTP2_SERVER_H2_STATIC_FILE_H
