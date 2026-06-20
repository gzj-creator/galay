/**
 * @file h2_static_file.h
 * @brief HTTP/2 static file metadata and small-file cache
 */

#ifndef GALAY_HTTP2_SERVER_H2_STATIC_FILE_H
#define GALAY_HTTP2_SERVER_H2_STATIC_FILE_H

#include "galay-http2/protoc/http2_hpack.h"

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
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

struct H2StaticFileLookup {
    int status = 404;
    std::filesystem::path file_path;
    uintmax_t file_size = 0;
    std::time_t last_modified = 0;
    std::string etag;
    std::string content_type = "application/octet-stream";
    bool body_cached = false;
    std::shared_ptr<const std::string> body;
    std::shared_ptr<const std::string> encoded_headers;
    std::vector<Http2HeaderField> headers;
    uintmax_t range_start = 0;
    uintmax_t range_end = 0;
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

private:
    struct Entry {
        std::filesystem::path file_path;
        uintmax_t file_size = 0;
        std::time_t last_modified = 0;
        std::string etag;
        std::string content_type;
        bool body_cached = false;
        std::shared_ptr<const std::string> body;
        std::shared_ptr<const std::string> encoded_headers;
        std::vector<Http2HeaderField> headers;
    };

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
