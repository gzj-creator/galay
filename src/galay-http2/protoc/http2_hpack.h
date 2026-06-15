/**
 * @file http2_hpack.h
 * @brief HTTP/2 HPACK 头部压缩编解码
 * @author galay-http
 * @version 1.0.0
 *
 * @details 实现 RFC 7541 HPACK 头部压缩算法，包括静态表、动态表、
 *          霍夫曼编解码，以及头部构建器（Http2Headers）。
 */

#ifndef GALAY_HTTP2_HPACK_H
#define GALAY_HTTP2_HPACK_H

#include "http2_base.h"
#include "http2_error.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <expected>
#include <optional>

namespace galay::http2
{

/**
 * @brief HTTP/2 头部字段
 */
struct Http2HeaderField
{
    std::string name;
    std::string value;

    bool operator==(const Http2HeaderField& other) const {
        return name == other.name && value == other.value;
    }

    size_t size() const {
        // RFC 7541: 头部字段大小 = 名称长度 + 值长度 + 32
        return name.size() + value.size() + 32;
    }
};

/**
 * @brief HTTP/2 头部构建器，支持链式调用
 * @details 通过隐式转换兼容所有接受 const std::vector<Http2HeaderField>& 的 API
 */
class Http2Headers
{
public:
    Http2Headers() = default;

    // 通用添加
    Http2Headers& add(std::string name, std::string value) {
        m_fields.push_back({std::move(name), std::move(value)});
        return *this;
    }

    // 请求伪头部
    Http2Headers& method(const std::string& m)     { return add(":method", m); }
    Http2Headers& scheme(const std::string& s)     { return add(":scheme", s); }
    Http2Headers& authority(const std::string& a)  { return add(":authority", a); }
    Http2Headers& path(const std::string& p)       { return add(":path", p); }

    // 响应伪头部
    Http2Headers& status(int code) { return add(":status", std::to_string(code)); }

    // 常用头部快捷方法
    Http2Headers& contentType(const std::string& ct) { return add("content-type", ct); }
    Http2Headers& contentLength(size_t len)           { return add("content-length", std::to_string(len)); }
    Http2Headers& server(const std::string& s)        { return add("server", s); }

    // 隐式转换为 vector，兼容现有 API
    operator const std::vector<Http2HeaderField>&() const { return m_fields; }
    const std::vector<Http2HeaderField>& fields() const { return m_fields; }

private:
    std::vector<Http2HeaderField> m_fields;
};

/**
 * @brief HPACK 静态表
 * @details RFC 7541 Appendix A 定义的静态表
 */
class HpackStaticTable
{
public:
    static const HpackStaticTable& instance();

    // 获取静态表条目（索引从 1 开始）
    const Http2HeaderField* get(size_t index) const;

    // 查找头部字段在静态表中的索引
    // 返回 {index, name_only}，name_only 表示只匹配了名称
    std::pair<size_t, bool> find(const std::string& name, const std::string& value) const;

    // 静态表大小
    size_t size() const { return m_table.size(); }

private:
    HpackStaticTable();
    std::vector<Http2HeaderField> m_table;
    // name -> static table indices (1-based), in declaration order.
    std::unordered_map<std::string, std::vector<size_t>> m_name_to_indices;
};

/**
 * @brief HPACK 动态表
 */
class HpackDynamicTable
{
public:
    HpackDynamicTable(size_t max_size = kDefaultHeaderTableSize);

    // 添加条目到动态表头部
    void add(const Http2HeaderField& field);

    // 获取条目（索引从 0 开始，0 是最新的）
    const Http2HeaderField* get(size_t index) const;

    // 查找头部字段
    std::pair<size_t, bool> find(const std::string& name, const std::string& value) const;

    // 设置最大大小
    void setMaxSize(size_t max_size);

    // 获取当前大小
    size_t currentSize() const { return m_current_size; }

    // 获取最大大小
    size_t maxSize() const { return m_max_size; }

    // 获取条目数量
    size_t count() const { return m_count; }

    // 清空动态表
    void clear();

private:
    void evict();

    std::vector<Http2HeaderField> m_ring;  // 环形缓冲区
    size_t m_head = 0;    // 下一个写入位置（最新条目的后一位）
    size_t m_count = 0;   // 当前条目数
    size_t m_max_size;
    size_t m_current_size = 0;
};

/**
 * @brief HPACK 编码器
 */
class HpackEncoder
{
public:
    HpackEncoder(size_t max_table_size = kDefaultHeaderTableSize);

    // 编码头部列表
    std::string encode(const std::vector<Http2HeaderField>& headers);
    std::string encodeStateless(const std::vector<Http2HeaderField>& headers);

    // 编码单个头部字段
    void encodeField(const Http2HeaderField& field, std::string& output);

    // 设置动态表最大大小
    void setMaxTableSize(size_t size);

    // 获取动态表
    HpackDynamicTable& dynamicTable() { return m_dynamic_table; }

private:
    // 编码整数
    static void encodeInteger(uint32_t value, uint8_t prefix_bits, uint8_t prefix, std::string& output);

    // 编码字符串（可选 Huffman 编码）
    void encodeString(const std::string& str, bool use_huffman, std::string& output);

    // 编码索引头部字段
    void encodeIndexed(size_t index, std::string& output);

    // 编码带索引的字面头部字段
    void encodeLiteralIndexed(size_t name_index, const std::string& value, std::string& output);
    void encodeLiteralIndexed(const std::string& name, const std::string& value, std::string& output);

    // 编码不带索引的字面头部字段
    void encodeLiteralWithoutIndexing(size_t name_index, const std::string& value, std::string& output);
    void encodeLiteralWithoutIndexing(const std::string& name, const std::string& value, std::string& output);

    // 编码永不索引的字面头部字段
    void encodeLiteralNeverIndexed(size_t name_index, const std::string& value, std::string& output);
    void encodeLiteralNeverIndexed(const std::string& name, const std::string& value, std::string& output);
    void encodeFieldStateless(const Http2HeaderField& field, std::string& output);

    HpackDynamicTable m_dynamic_table;
    bool m_use_huffman = false;  // 是否使用 Huffman 编码
    bool m_table_size_update_pending = false;
    size_t m_pending_table_size = 0;
};

/**
 * @brief HPACK 解码器
 */
class HpackDecoder
{
public:
    HpackDecoder(size_t max_table_size = kDefaultHeaderTableSize);

    // 解码头部块
    std::expected<std::vector<Http2HeaderField>, Http2ErrorCode> decode(const uint8_t* data, size_t length);
    std::expected<std::vector<Http2HeaderField>, Http2ErrorCode> decode(const std::string& data) {
        return decode(reinterpret_cast<const uint8_t*>(data.data()), data.size());
    }

    // 设置动态表最大大小
    void setMaxTableSize(size_t size);
    // 设置头部列表最大大小（RFC 7541 字段大小累计）
    void setMaxHeaderListSize(size_t size);
    size_t maxHeaderListSize() const { return m_max_header_list_size; }

    // 获取动态表
    HpackDynamicTable& dynamicTable() { return m_dynamic_table; }

private:
    // 解码整数
    static std::expected<uint32_t, Http2ErrorCode> decodeInteger(const uint8_t*& data, const uint8_t* end, uint8_t prefix_bits);

    // 解码字符串
    std::expected<std::string, Http2ErrorCode> decodeString(const uint8_t*& data, const uint8_t* end);

    // 从索引获取头部字段
    const Http2HeaderField* getField(size_t index) const;

    HpackDynamicTable m_dynamic_table;
    size_t m_max_table_size;
    size_t m_max_header_list_size = static_cast<size_t>(-1);
};

/**
 * @brief Huffman 编码/解码
 */
class HpackHuffman
{
public:
    // Huffman 编码
    static std::string encode(const std::string& input);

    // Huffman 解码
    static std::expected<std::string, Http2ErrorCode> decode(const uint8_t* data, size_t length);

    // 计算 Huffman 编码后的长度
    static size_t encodedLength(const std::string& input);
};

} // namespace galay::http2

#endif // GALAY_HTTP2_HPACK_H
