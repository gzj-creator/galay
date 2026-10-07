#include "h2_static_file.h"

#include "../../galay-http/protoc/http_base.h"
#include "../../galay-http/server/http_etag.h"
#include "../../galay-http/server/http_range.h"

#include <chrono>
#include <sstream>
#include <string_view>

namespace galay::http2
{

namespace {

std::time_t to_time_t(std::filesystem::file_time_type file_time)
{
    const auto system_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        file_time - std::filesystem::file_time_type::clock::now() +
        std::chrono::system_clock::now());
    return std::chrono::system_clock::to_time_t(system_time);
}

std::string extension_to_mime(const std::filesystem::path& path)
{
    auto ext = path.extension().string();
    if (!ext.empty() && ext.front() == '.') {
        return galay::http::MimeType::convert_to_mime_type(ext.substr(1));
    }
    return galay::http::MimeType::convert_to_mime_type(ext);
}

std::string strip_query(std::string_view path)
{
    const auto query_pos = path.find('?');
    if (query_pos == std::string_view::npos) {
        return std::string(path);
    }
    return std::string(path.substr(0, query_pos));
}

} // namespace

H2StaticFileMount make_h2_static_file_mount(std::string prefix, H2StaticFileConfig config)
{
    if (prefix.empty()) {
        prefix = "/";
    }
    if (prefix.front() != '/') {
        prefix = "/" + prefix;
    }
    while (prefix.size() > 1 && prefix.back() == '/') {
        prefix.pop_back();
    }
    H2StaticFileMount mount;
    mount.prefix = std::move(prefix);
    mount.config = std::move(config);
    mount.cache = std::make_shared<H2StaticFileCache>(mount.config);
    return mount;
}

std::shared_ptr<const std::string> encode_h2_static_file_headers(
    int status,
    const std::vector<Http2HeaderField>& headers)
{
    std::vector<Http2HeaderField> fields;
    fields.reserve(headers.size() + 1);
    fields.push_back({":status", std::to_string(status)});
    fields.insert(fields.end(), headers.begin(), headers.end());
    HpackEncoder encoder;
    auto block = encoder.encode_stateless(fields);
    return std::make_shared<const std::string>(std::move(block));
}

H2StaticFileCache::H2StaticFileCache(H2StaticFileConfig config)
    : m_config(std::move(config))
{
    std::error_code ec;
    m_root = std::filesystem::weakly_canonical(m_config.root, ec);
    if (ec) {
        m_root = std::filesystem::absolute(m_config.root, ec);
    }
}

H2StaticFileLookup H2StaticFileCache::lookup(const H2StaticFileRequest& request)
{
    auto* entry = find_or_load_entry(request.path);
    if (entry == nullptr) {
        return make_not_found();
    }

    if (m_config.enable_etag &&
        !request.if_none_match.empty() &&
        galay::http::ETagGenerator::match_if_none_match(entry->etag, request.if_none_match)) {
        return make_lookup(*entry, 304);
    }

    if (!request.range.empty()) {
        auto range_result = galay::http::HttpRangeParser::parse(request.range, entry->file_size);
        if (!range_result.is_valid() ||
            range_result.type != galay::http::RangeType::SINGLE_RANGE) {
            auto lookup = make_lookup(*entry, 416);
            lookup.headers.clear();
            lookup.headers.push_back({"content-length", "0"});
            lookup.headers.push_back({"content-range", "bytes */" + std::to_string(entry->file_size)});
            if (m_config.enable_etag && !entry->etag.empty()) {
                lookup.headers.push_back({"etag", entry->etag});
            }
            lookup.body = nullptr;
            lookup.body_cached = false;
            return lookup;
        }

        const auto& range = range_result.ranges.front();
        auto lookup = make_lookup(*entry, 206);
        lookup.range_start = range.start;
        lookup.range_end = range.end;
        const auto length = range.end - range.start + 1;
        lookup.headers.clear();
        lookup.headers.push_back({"content-length", std::to_string(length)});
        lookup.headers.push_back({"content-type", entry->content_type});
        lookup.headers.push_back({
            "content-range",
            galay::http::HttpRangeParser::make_content_range(range, entry->file_size)});
        lookup.headers.push_back({"accept-ranges", "bytes"});
        if (m_config.enable_etag && !entry->etag.empty()) {
            lookup.headers.push_back({"etag", entry->etag});
        }
        auto cached_body = entry->body_cache_slot ? entry->body_cache_slot->load() : nullptr;
        if (cached_body) {
            lookup.body = std::make_shared<const std::string>(
                cached_body->substr(static_cast<size_t>(range.start), static_cast<size_t>(length)));
            lookup.body_cached = true;
        } else {
            lookup.body = nullptr;
            lookup.body_cached = false;
        }
        lookup.body_cache_slot = entry->body_cache_slot;
        lookup.body_cacheable = entry->body_cacheable;
        return lookup;
    }
    return make_lookup(*entry, 200);
}

std::optional<H2StaticFileFastLookup> H2StaticFileCache::lookup_fast200(
    std::string_view request_path)
{
    auto* entry = find_or_load_entry(request_path);
    if (entry == nullptr) {
        return std::nullopt;
    }

    H2StaticFileFastLookup lookup;
    auto cached_body = entry->body_cache_slot ? entry->body_cache_slot->load() : nullptr;
    lookup.file_path = entry->file_path;
    lookup.content_length = entry->file_size;
    lookup.body_cached = static_cast<bool>(cached_body);
    lookup.body = std::move(cached_body);
    lookup.encoded_headers = entry->encoded_headers;
    lookup.body_cache_slot = entry->body_cache_slot;
    lookup.body_cacheable = entry->body_cacheable;
    return lookup;
}

H2StaticFileCache::Entry* H2StaticFileCache::find_or_load_entry(std::string_view request_path)
{
    const auto cache_path = strip_query(request_path);
    std::string key;
    if (auto cached = m_request_path_cache.find(cache_path);
        cached != m_request_path_cache.end()) {
        key = cached->second;
    } else {
        auto file_path = normalize_request_path(cache_path);
        if (file_path.empty()) {
            return nullptr;
        }
        key = file_path.string();
        auto [_, inserted] = m_request_path_cache.emplace(cache_path, key);
        if (!inserted) {
            m_request_path_cache[cache_path] = key;
        }
    }

    auto it = m_cache.find(key);
    if (it == m_cache.end()) {
        std::filesystem::path file_path(key);
        std::error_code ec;
        if (!std::filesystem::is_regular_file(file_path, ec) || ec) {
            return nullptr;
        }
        auto [insert_it, inserted] = m_cache.emplace(key, load_entry(file_path));
        it = insert_it;
        if (!inserted) {
            return &it->second;
        }
    }
    return &it->second;
}

std::filesystem::path H2StaticFileCache::normalize_request_path(
    const std::string& request_path) const
{
    if (request_path.empty() || request_path.find('\0') != std::string::npos) {
        return {};
    }

    std::string relative = request_path;
    const auto query_pos = relative.find('?');
    if (query_pos != std::string::npos) {
        relative.resize(query_pos);
    }
    while (!relative.empty() && relative.front() == '/') {
        relative.erase(relative.begin());
    }
    if (relative.empty()) {
        relative = "index.html";
    }

    std::error_code ec;
    auto candidate = (m_root / relative).lexically_normal();
    if (!is_inside_root(candidate)) {
        return {};
    }
    if (!std::filesystem::exists(candidate, ec) || ec) {
        return {};
    }
    auto canonical = std::filesystem::weakly_canonical(candidate, ec);
    if (ec || !is_inside_root(canonical)) {
        return {};
    }
    return canonical;
}

bool H2StaticFileCache::is_inside_root(const std::filesystem::path& path) const
{
    std::error_code ec;
    auto relative = std::filesystem::relative(path, m_root, ec);
    if (ec || relative.empty()) {
        return false;
    }
    for (const auto& part : relative) {
        if (part == "..") {
            return false;
        }
    }
    return true;
}

H2StaticFileLookup H2StaticFileCache::make_not_found() const
{
    H2StaticFileLookup lookup;
    lookup.status = 404;
    lookup.headers.push_back({"content-length", "0"});
    return lookup;
}

H2StaticFileLookup H2StaticFileCache::make_lookup(const Entry& entry, int status) const
{
    H2StaticFileLookup lookup;
    lookup.status = status;
    lookup.file_path = entry.file_path;
    lookup.file_size = entry.file_size;
    lookup.last_modified = entry.last_modified;
    lookup.etag = entry.etag;
    lookup.content_type = entry.content_type;
    lookup.body_cache_slot = entry.body_cache_slot;
    lookup.body_cacheable = entry.body_cacheable;
    auto cached_body = entry.body_cache_slot ? entry.body_cache_slot->load() : nullptr;
    lookup.body_cached = status == 200 && static_cast<bool>(cached_body);
    lookup.body = lookup.body_cached ? std::move(cached_body) : nullptr;
    if (status == 200) {
        lookup.headers = entry.headers;
        lookup.encoded_headers = entry.encoded_headers;
    } else {
        lookup.headers.push_back({"content-length", std::to_string(entry.file_size)});
        lookup.headers.push_back({"content-type", entry.content_type});
        lookup.headers.push_back({"accept-ranges", "bytes"});
        if (m_config.enable_etag && !entry.etag.empty()) {
            lookup.headers.push_back({"etag", entry.etag});
        }
    }
    return lookup;
}

H2StaticFileCache::Entry H2StaticFileCache::load_entry(
    const std::filesystem::path& file_path) const
{
    Entry entry;
    entry.file_path = file_path;

    std::error_code ec;
    entry.file_size = std::filesystem::file_size(file_path, ec);
    if (ec) {
        entry.file_size = 0;
    }
    entry.last_modified = to_time_t(std::filesystem::last_write_time(file_path, ec));
    if (ec) {
        entry.last_modified = 0;
    }
    entry.content_type = extension_to_mime(file_path);
    if (m_config.enable_etag) {
        entry.etag = galay::http::ETagGenerator::generate_strong(
            file_path, static_cast<size_t>(entry.file_size), entry.last_modified);
    }
    entry.headers.push_back({"content-length", std::to_string(entry.file_size)});
    entry.headers.push_back({"content-type", entry.content_type});
    entry.headers.push_back({"accept-ranges", "bytes"});
    if (m_config.enable_etag && !entry.etag.empty()) {
        entry.headers.push_back({"etag", entry.etag});
    }
    entry.encoded_headers = encode_h2_static_file_headers(200, entry.headers);
    if (entry.file_size <= m_config.small_file_threshold) {
        entry.body_cache_slot = std::make_shared<H2StaticFileBodyCacheSlot>();
        entry.body_cacheable = true;
    }
    return entry;
}

} // namespace galay::http2
