#include "ws_frame.h"
#include "../common/macro.hpp"

#include "../utils/ws_helper.h"

#include <algorithm>
#include <cstring>
#include <limits>

#if defined(GALAY_WS_SIMD_X86)
    #include <immintrin.h>
#elif defined(GALAY_WS_SIMD_NEON)
    #include <arm_neon.h>
#endif

namespace galay::websocket
{

namespace {

void apply_mask_bytes_impl(char* data, size_t len, const uint8_t masking_key[4]) {
    if (data == nullptr || len == 0) {
        return;
    }

    size_t i = 0;
    uint8_t* ptr = reinterpret_cast<uint8_t*>(data);

#if defined(GALAY_WS_SIMD_NEON)
    if (len >= 16) {
        uint8_t mask_array[16];
        for (int j = 0; j < 16; ++j) {
            mask_array[j] = masking_key[j % 4];
        }
        uint8x16_t mask_vec = vld1q_u8(mask_array);

        for (; i + 16 <= len; i += 16) {
            uint8x16_t data_vec = vld1q_u8(ptr + i);
            uint8x16_t result = veorq_u8(data_vec, mask_vec);
            vst1q_u8(ptr + i, result);
        }
    }
#elif defined(GALAY_WS_SIMD_X86)
    if (len >= 16) {
        uint32_t mask32;
        // memcpy 支持未对齐的 key；返回的目标地址无需使用。
        (void)std::memcpy(&mask32, masking_key, sizeof(mask32));
        __m128i mask_vec = _mm_set1_epi32(static_cast<int>(mask32));

        for (; i + 16 <= len; i += 16) {
            __m128i data_vec = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ptr + i));
            __m128i result = _mm_xor_si128(data_vec, mask_vec);
            _mm_storeu_si128(reinterpret_cast<__m128i*>(ptr + i), result);
        }
    }
#endif

    if (i + 8 <= len) {
        uint64_t mask64;
        std::memcpy(&mask64, masking_key, 4);
        std::memcpy(reinterpret_cast<uint8_t*>(&mask64) + 4, masking_key, 4);

        for (; i + 8 <= len; i += 8) {
            uint64_t chunk;
            std::memcpy(&chunk, ptr + i, sizeof(chunk));
            chunk ^= mask64;
            std::memcpy(ptr + i, &chunk, sizeof(chunk));
        }
    }

    if (i + 4 <= len) {
        uint32_t mask32;
        std::memcpy(&mask32, masking_key, 4);
        uint32_t chunk;
        std::memcpy(&chunk, ptr + i, sizeof(chunk));
        chunk ^= mask32;
        std::memcpy(ptr + i, &chunk, sizeof(chunk));
        i += 4;
    }

    for (; i < len; ++i) {
        ptr[i] ^= masking_key[i % 4];
    }
}

class IovecCursor {
public:
    IovecCursor(const iovec* iovecs, size_t iovec_count, size_t total_length)
        : m_iovecs(iovecs)
        , m_iov_count(iovec_count)
        , m_total_length(total_length)
        , m_iov_index(0)
        , m_iov_offset(0)
        , m_consumed(0)
    {
    }

    size_t consumed() const {
        return m_consumed;
    }

    size_t remaining() const {
        return m_total_length - m_consumed;
    }

    bool read_byte(uint8_t& out) {
        return read_bytes(&out, 1);
    }

    bool read_bytes(void* dst, size_t len) {
        uint8_t* out = static_cast<uint8_t*>(dst);
        size_t need = len;

        while (need > 0) {
            if (m_iovecs == nullptr || m_iov_index >= m_iov_count) {
                return false;
            }

            const auto& iov = m_iovecs[m_iov_index];
            if (m_iov_offset >= iov.iov_len) {
                ++m_iov_index;
                m_iov_offset = 0;
                continue;
            }

            const size_t available = iov.iov_len - m_iov_offset;
            const size_t take = std::min(available, need);

            std::memcpy(out,
                       static_cast<const uint8_t*>(iov.iov_base) + m_iov_offset,
                       take);

            out += take;
            need -= take;
            m_iov_offset += take;
            m_consumed += take;

            if (m_iov_offset == iov.iov_len) {
                ++m_iov_index;
                m_iov_offset = 0;
            }
        }

        return true;
    }

private:
    const iovec* m_iovecs;
    size_t m_iov_count;
    size_t m_total_length;
    size_t m_iov_index;
    size_t m_iov_offset;
    size_t m_consumed;
};

} // namespace

std::expected<size_t, WsError>
WsFrameParser::from_io_vec(const std::vector<iovec>& iovecs, WsFrame& frame, bool is_server)
{
    return from_io_vec(iovecs.data(), iovecs.size(), frame, is_server);
}

std::expected<size_t, WsError>
WsFrameParser::from_io_vec(const struct iovec* iovecs, size_t iovec_count, WsFrame& frame, bool is_server)
{
    const size_t total_length = get_total_length(iovecs, iovec_count);
    if (total_length < 2) {
        return std::unexpected(WsError(kWsIncomplete));
    }

    IovecCursor cursor(iovecs, iovec_count, total_length);
    uint8_t byte1 = 0;
    uint8_t byte2 = 0;

    if (!cursor.read_byte(byte1)) {
        return std::unexpected(WsError(kWsIncomplete));
    }

    // 解析FIN和RSV位
    frame.header.fin = (byte1 & 0x80) != 0;
    frame.header.rsv1 = (byte1 & 0x40) != 0;
    frame.header.rsv2 = (byte1 & 0x20) != 0;
    frame.header.rsv3 = (byte1 & 0x10) != 0;

    // 检查保留位（如果没有协商扩展，保留位必须为0）
    if (frame.header.rsv1 || frame.header.rsv2 || frame.header.rsv3) {
        return std::unexpected(WsError(kWsReservedBitsSet));
    }

    // 解析操作码
    uint8_t opcode_value = byte1 & 0x0F;
    if (opcode_value > 0x0A || (opcode_value > 0x02 && opcode_value < 0x08)) {
        return std::unexpected(WsError(kWsInvalidOpcode));
    }
    frame.header.opcode = static_cast<WsOpcode>(opcode_value);

    // 控制帧必须设置FIN位
    if (is_control_frame(frame.header.opcode) && !frame.header.fin) {
        return std::unexpected(WsError(kWsControlFrameFragmented));
    }

    if (!cursor.read_byte(byte2)) {
        return std::unexpected(WsError(kWsIncomplete));
    }

    // 解析MASK位
    frame.header.mask = (byte2 & 0x80) != 0;

    // 服务器端要求客户端必须使用掩码
    if (is_server && !frame.header.mask) {
        return std::unexpected(WsError(kWsMaskRequired));
    }

    // 客户端不应该收到带掩码的帧
    if (!is_server && frame.header.mask) {
        return std::unexpected(WsError(kWsMaskNotAllowed));
    }

    // 解析payload长度
    uint8_t payload_len = byte2 & 0x7F;

    if (payload_len < 126) {
        frame.header.payload_length = payload_len;
    } else if (payload_len == 126) {
        uint8_t len_buf[2];
        if (!cursor.read_bytes(len_buf, sizeof(len_buf))) {
            return std::unexpected(WsError(kWsIncomplete));
        }
        const uint16_t extended_len = (static_cast<uint16_t>(len_buf[0]) << 8) | len_buf[1];
        if (extended_len < 126) {
            return std::unexpected(WsError(kWsInvalidPayloadLength));
        }
        frame.header.payload_length = extended_len;
    } else {
        uint8_t len_buf[8];
        if (!cursor.read_bytes(len_buf, sizeof(len_buf))) {
            return std::unexpected(WsError(kWsIncomplete));
        }
        if ((len_buf[0] & 0x80) != 0) {
            return std::unexpected(WsError(kWsInvalidPayloadLength));
        }
        uint64_t extended_len = 0;
        for (size_t i = 0; i < sizeof(len_buf); ++i) {
            extended_len = (extended_len << 8) | len_buf[i];
        }
        if (extended_len <= 0xFFFF) {
            return std::unexpected(WsError(kWsInvalidPayloadLength));
        }
        frame.header.payload_length = extended_len;
    }

    // 控制帧的payload不能超过125字节
    if (is_control_frame(frame.header.opcode) && frame.header.payload_length > 125) {
        return std::unexpected(WsError(kWsControlFrameTooLarge));
    }

    // 读取掩码密钥（如果有）
    if (frame.header.mask) {
        if (!cursor.read_bytes(frame.header.masking_key, sizeof(frame.header.masking_key))) {
            return std::unexpected(WsError(kWsIncomplete));
        }
    }

    if (cursor.remaining() < frame.header.payload_length) {
        return std::unexpected(WsError(kWsIncomplete));
    }

    if (frame.header.payload_length > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
        return std::unexpected(WsError(kWsInvalidFrame));
    }
    const size_t payload_size = static_cast<size_t>(frame.header.payload_length);

    frame.payload.clear();
    frame.payload.resize(payload_size);
    if (payload_size > 0 && !cursor.read_bytes(frame.payload.data(), payload_size)) {
        return std::unexpected(WsError(kWsInvalidFrame));
    }

    // 如果有掩码，解除掩码
    if (frame.header.mask) {
        apply_mask(frame.payload, frame.header.masking_key);
    }

    if (frame.header.opcode == WsOpcode::Close) {
        if (payload_size == 1) {
            return std::unexpected(WsError(kWsInvalidPayloadLength));
        }
        if (payload_size >= 2) {
            const uint16_t code =
                (static_cast<uint16_t>(static_cast<uint8_t>(frame.payload[0])) << 8) |
                static_cast<uint8_t>(frame.payload[1]);
            const bool valid_code =
                (code >= 1000 && code <= 4999) &&
                code != 1004 &&
                code != 1005 &&
                code != 1006 &&
                code != 1015;
            if (!valid_code) {
                return std::unexpected(WsError(kWsInvalidCloseCode));
            }
            if (payload_size > 2 &&
                !is_valid_utf8_bytes(frame.payload.data() + 2, payload_size - 2)) {
                return std::unexpected(WsError(kWsInvalidUtf8));
            }
        }
    }

    // 验证文本帧的UTF-8编码
    if (frame.header.opcode == WsOpcode::Text && frame.header.fin) {
        if (!is_valid_utf8(frame.payload)) {
            return std::unexpected(WsError(kWsInvalidUtf8));
        }
    }

    return cursor.consumed();
}

std::string WsFrameParser::to_bytes(const WsFrame& frame, bool use_mask)
{
    std::string result;
    encode_into(result, frame, use_mask);
    return result;
}

void WsFrameParser::encode_into(std::string& out, const WsFrame& frame, bool use_mask)
{
    const uint64_t payload_len = frame.payload.size();
    const size_t header_len = ws_frame_header_length(payload_len, use_mask);
    uint8_t masking_key[4] = {0, 0, 0, 0};

    out.clear();
    out.reserve(header_len + static_cast<size_t>(payload_len));
    append_ws_frame_header(out, frame, payload_len, use_mask, masking_key);
    if (payload_len == 0) {
        return;
    }

    const size_t payload_offset = out.size();
    out.append(frame.payload.data(), frame.payload.size());
    if (use_mask) {
        WsFrameParser::apply_mask_bytes(out.data() + payload_offset, static_cast<size_t>(payload_len), masking_key);
    }
}

void WsFrameParser::encode_message_into(std::string& out,
                                      WsOpcode opcode,
                                      std::string_view payload,
                                      bool fin,
                                      bool use_mask)
{
    const uint64_t payload_len = payload.size();
    const size_t header_len = ws_frame_header_length(payload_len, use_mask);
    uint8_t masking_key[4] = {0, 0, 0, 0};

    out.clear();
    out.reserve(header_len + static_cast<size_t>(payload_len));
    append_ws_frame_header(out, opcode, fin, false, false, false, payload_len, use_mask, masking_key);
    if (payload_len == 0) {
        return;
    }

    const size_t payload_offset = out.size();
    out.append(payload.data(), payload.size());
    if (use_mask) {
        WsFrameParser::apply_mask_bytes(out.data() + payload_offset, static_cast<size_t>(payload_len), masking_key);
    }
}

void WsFrameParser::encode_message_into(std::string& out,
                                      WsOpcode opcode,
                                      std::string&& payload,
                                      bool fin,
                                      bool use_mask)
{
    const uint64_t payload_len = payload.size();
    const size_t header_len = ws_frame_header_length(payload_len, use_mask);
    uint8_t masking_key[4] = {0, 0, 0, 0};

    out.clear();
    out.swap(payload);

    std::string header;
    header.reserve(header_len);
    append_ws_frame_header(header, opcode, fin, false, false, false, payload_len, use_mask, masking_key);

    if (out.empty()) {
        out = std::move(header);
        return;
    }

    const size_t payload_size = out.size();
    out.reserve(header.size() + payload_size);
    out.resize(header.size() + payload_size);
    std::memmove(out.data() + header.size(), out.data(), payload_size);
    std::memcpy(out.data(), header.data(), header.size());
    if (use_mask) {
        WsFrameParser::apply_mask_bytes(out.data() + header.size(), static_cast<size_t>(payload_len), masking_key);
    }
}

std::string WsFrameParser::to_bytes_header(const WsFrame& frame, bool use_mask, uint8_t masking_key[4])
{
    std::string result;
    const uint64_t payload_len = frame.payload.size();
    const size_t header_len = ws_frame_header_length(payload_len, use_mask);
    result.reserve(header_len);
    append_ws_frame_header(result, frame, payload_len, use_mask, masking_key);
    return result;
}

WsFrame WsFrameParser::create_close_frame(WsCloseCode code, const std::string& reason)
{
    return WsFrameBuilder().close(code, reason).build_move();
}

void WsFrameParser::apply_mask_bytes(char* data, size_t len, const uint8_t masking_key[4])
{
    apply_mask_bytes_impl(data, len, masking_key);
}

void WsFrameParser::apply_mask(std::string& data, const uint8_t masking_key[4])
{
    apply_mask_bytes(data.data(), data.size(), masking_key);
}

bool WsFrameParser::is_valid_utf8_bytes(const char* data, size_t len)
{
    if (data == nullptr) {
        return len == 0;
    }

    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(data);
    size_t i = 0;

#if defined(GALAY_WS_SIMD_NEON)
    // ARM NEON 优化：快速检测 ASCII 字符（0x00-0x7F）
    if (len >= 16) {
        for (; i + 16 <= len; i += 16) {
            uint8x16_t chunk = vld1q_u8(ptr + i);
            // 检查是否所有字节都是 ASCII (最高位为 0)
            uint8x16_t high_bits = vandq_u8(chunk, vdupq_n_u8(0x80));
            // 如果有任何非 ASCII 字符，跳出 SIMD 处理
            uint64x2_t result = vreinterpretq_u64_u8(high_bits);
            if (vgetq_lane_u64(result, 0) != 0 || vgetq_lane_u64(result, 1) != 0) {
                break;
            }
        }
    }
#elif defined(GALAY_WS_SIMD_X86)
    // x86 SSE2 优化：快速检测 ASCII 字符
    if (len >= 16) {
        __m128i high_bit_mask = _mm_set1_epi8(0x80);
        for (; i + 16 <= len; i += 16) {
            __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ptr + i));
            __m128i high_bits = _mm_and_si128(chunk, high_bit_mask);
            // 如果有任何非 ASCII 字符，跳出 SIMD 处理
            if (_mm_movemask_epi8(high_bits) != 0) {
                break;
            }
        }
    }
#endif

    // 标量处理剩余字节和多字节 UTF-8 序列
    while (i < len) {
        uint8_t byte = ptr[i];

        if (byte <= 0x7F) {
            // 单字节字符 (0xxxxxxx)
            i++;
        } else if ((byte & 0xE0) == 0xC0) {
            // 双字节字符 (110xxxxx 10xxxxxx)
            if (i + 1 >= len) return false;
            uint8_t byte2 = ptr[i + 1];
            if ((byte2 & 0xC0) != 0x80) return false;

            // 检查过长编码：双字节序列的最小值是 0x80
            uint32_t codepoint = ((byte & 0x1F) << 6) | (byte2 & 0x3F);
            if (codepoint < 0x80) return false;

            i += 2;
        } else if ((byte & 0xF0) == 0xE0) {
            // 三字节字符 (1110xxxx 10xxxxxx 10xxxxxx)
            if (i + 2 >= len) return false;
            uint8_t byte2 = ptr[i + 1];
            uint8_t byte3 = ptr[i + 2];
            if ((byte2 & 0xC0) != 0x80) return false;
            if ((byte3 & 0xC0) != 0x80) return false;

            // 检查过长编码：三字节序列的最小值是 0x800
            uint32_t codepoint = ((byte & 0x0F) << 12) | ((byte2 & 0x3F) << 6) | (byte3 & 0x3F);
            if (codepoint < 0x800) return false;

            // 检查代理对范围 (U+D800 到 U+DFFF 是无效的)
            if (codepoint >= 0xD800 && codepoint <= 0xDFFF) return false;

            i += 3;
        } else if ((byte & 0xF8) == 0xF0) {
            // 四字节字符 (11110xxx 10xxxxxx 10xxxxxx 10xxxxxx)
            if (i + 3 >= len) return false;
            uint8_t byte2 = ptr[i + 1];
            uint8_t byte3 = ptr[i + 2];
            uint8_t byte4 = ptr[i + 3];
            if ((byte2 & 0xC0) != 0x80) return false;
            if ((byte3 & 0xC0) != 0x80) return false;
            if ((byte4 & 0xC0) != 0x80) return false;

            // 检查过长编码：四字节序列的最小值是 0x10000
            uint32_t codepoint = ((byte & 0x07) << 18) | ((byte2 & 0x3F) << 12) |
                                ((byte3 & 0x3F) << 6) | (byte4 & 0x3F);
            if (codepoint < 0x10000) return false;

            // 检查最大值：Unicode 最大码点是 U+10FFFF
            if (codepoint > 0x10FFFF) return false;

            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

bool WsFrameParser::is_valid_utf8_masked_bytes(const char* data,
                                           size_t len,
                                           const uint8_t masking_key[4])
{
    if (data == nullptr) {
        return len == 0;
    }

    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(data);
    size_t i = 0;

#if defined(GALAY_WS_SIMD_NEON)
    if (len >= 16) {
        uint8_t mask_array[16];
        for (int j = 0; j < 16; ++j) {
            mask_array[j] = masking_key[j % 4];
        }
        uint8x16_t mask_vec = vld1q_u8(mask_array);
        const uint8x16_t high_bit_mask = vdupq_n_u8(0x80);

        for (; i + 16 <= len; i += 16) {
            const uint8x16_t chunk = vld1q_u8(ptr + i);
            const uint8x16_t unmasked = veorq_u8(chunk, mask_vec);
            const uint8x16_t high_bits = vandq_u8(unmasked, high_bit_mask);
            const uint64x2_t result = vreinterpretq_u64_u8(high_bits);
            if (vgetq_lane_u64(result, 0) != 0 || vgetq_lane_u64(result, 1) != 0) {
                break;
            }
        }
    }
#elif defined(GALAY_WS_SIMD_X86)
    if (len >= 16) {
        uint8_t mask_array[16];
        for (int j = 0; j < 16; ++j) {
            mask_array[j] = masking_key[j % 4];
        }
        const __m128i mask_vec = _mm_loadu_si128(reinterpret_cast<const __m128i*>(mask_array));
        const __m128i high_bit_mask = _mm_set1_epi8(0x80);

        for (; i + 16 <= len; i += 16) {
            const __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(ptr + i));
            const __m128i unmasked = _mm_xor_si128(chunk, mask_vec);
            const __m128i high_bits = _mm_and_si128(unmasked, high_bit_mask);
            if (_mm_movemask_epi8(high_bits) != 0) {
                break;
            }
        }
    }
#endif

    const auto masked = [&](size_t index) noexcept -> uint8_t {
        return ptr[index] ^ masking_key[index % 4];
    };

    while (i < len) {
        const uint8_t byte = masked(i);

        if (byte <= 0x7F) {
            ++i;
        } else if ((byte & 0xE0) == 0xC0) {
            if (i + 1 >= len) return false;
            const uint8_t byte2 = masked(i + 1);
            if ((byte2 & 0xC0) != 0x80) return false;
            const uint32_t codepoint = ((byte & 0x1F) << 6) | (byte2 & 0x3F);
            if (codepoint < 0x80) return false;
            i += 2;
        } else if ((byte & 0xF0) == 0xE0) {
            if (i + 2 >= len) return false;
            const uint8_t byte2 = masked(i + 1);
            const uint8_t byte3 = masked(i + 2);
            if ((byte2 & 0xC0) != 0x80) return false;
            if ((byte3 & 0xC0) != 0x80) return false;
            const uint32_t codepoint = ((byte & 0x0F) << 12) |
                                       ((byte2 & 0x3F) << 6) |
                                       (byte3 & 0x3F);
            if (codepoint < 0x800) return false;
            if (codepoint >= 0xD800 && codepoint <= 0xDFFF) return false;
            i += 3;
        } else if ((byte & 0xF8) == 0xF0) {
            if (i + 3 >= len) return false;
            const uint8_t byte2 = masked(i + 1);
            const uint8_t byte3 = masked(i + 2);
            const uint8_t byte4 = masked(i + 3);
            if ((byte2 & 0xC0) != 0x80) return false;
            if ((byte3 & 0xC0) != 0x80) return false;
            if ((byte4 & 0xC0) != 0x80) return false;
            const uint32_t codepoint = ((byte & 0x07) << 18) |
                                       ((byte2 & 0x3F) << 12) |
                                       ((byte3 & 0x3F) << 6) |
                                       (byte4 & 0x3F);
            if (codepoint < 0x10000 || codepoint > 0x10FFFF) return false;
            i += 4;
        } else {
            return false;
        }
    }
    return true;
}

bool WsFrameParser::is_valid_utf8(const std::string& data)
{
    return is_valid_utf8_bytes(data.data(), data.size());
}

size_t WsFrameParser::get_total_length(const std::vector<iovec>& iovecs)
{
    return get_total_length(iovecs.data(), iovecs.size());
}

size_t WsFrameParser::get_total_length(const struct iovec* iovecs, size_t iovec_count)
{
    if (iovecs == nullptr || iovec_count == 0) {
        return 0;
    }
    if (iovec_count == 1) {
        return iovecs[0].iov_len;
    }
    if (iovec_count == 2) {
        return iovecs[0].iov_len + iovecs[1].iov_len;
    }

    size_t total = 0;
    for (size_t i = 0; i < iovec_count; ++i) {
        total += iovecs[i].iov_len;
    }
    return total;
}

} // namespace galay::websocket
