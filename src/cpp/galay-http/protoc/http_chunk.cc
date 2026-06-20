#include "http_chunk.h"
#include <charconv>
#include <sstream>
#include <iomanip>
#include <limits>

// SIMD 支持检测
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    #include <immintrin.h>
    #define GALAY_HTTP_SIMD_X86
#elif defined(__ARM_NEON) || defined(__aarch64__)
    #include <arm_neon.h>
    #define GALAY_HTTP_SIMD_NEON
#endif
#include <algorithm>

namespace galay::http
{
namespace {

constexpr size_t kMaxChunkLineSize = 1024;

std::expected<size_t, HttpError> parseChunkSizeLine(const std::string& size_buffer)
{
    const size_t extension_pos = size_buffer.find(';');
    std::string_view size_text(size_buffer.data(),
                               extension_pos == std::string::npos ? size_buffer.size() : extension_pos);
    if (size_text.empty()) {
        return std::unexpected(HttpError(kChunkSizeConvertError));
    }

    size_t parsed_size = 0;
    const char* begin = size_text.data();
    const char* end = begin + size_text.size();
    auto [ptr, ec] = std::from_chars(begin, end, parsed_size, 16);
    if (ec == std::errc::result_out_of_range) {
        return std::unexpected(HttpError(kInvalidChunkLength));
    }
    if (ec != std::errc{} || ptr != end) {
        return std::unexpected(HttpError(kChunkSizeConvertError));
    }
    return parsed_size;
}

bool wouldExceedBodyLimit(size_t body_size, size_t append_size, size_t max_body_size)
{
    if (max_body_size == 0) {
        return false;
    }
    if (body_size > max_body_size) {
        return true;
    }
    return append_size > max_body_size - body_size;
}

} // namespace

std::expected<std::pair<bool, size_t>, HttpError>
Chunk::fromIOVec(const std::vector<iovec>& iovecs, std::string& chunk_data)
{
    size_t total_consumed = 0;
    size_t iov_idx = 0;
    size_t byte_idx = 0;
    bool has_parsed_chunk = false;

    // 循环解析所有可用的完整chunk
    while (iov_idx < iovecs.size()) {
        // 解析chunk size行：HEX\r\n
        std::string size_buffer;
        size_t consumed = 0;

        if (!findCRLF(iovecs, iov_idx, byte_idx, size_buffer, consumed)) {
            // 没有找到完整的size行，数据不完整
            if (has_parsed_chunk) {
                // 已经解析了至少一个chunk，返回已消费的字节数
                return std::pair<bool, size_t>{false, total_consumed};
            }
            return std::unexpected(HttpError(kIncomplete));
        }

        // 更新位置
        total_consumed += consumed;
        size_t remaining = consumed;
        while (remaining > 0 && iov_idx < iovecs.size()) {
            size_t available = iovecs[iov_idx].iov_len - byte_idx;
            if (available <= remaining) {
                remaining -= available;
                iov_idx++;
                byte_idx = 0;
            } else {
                byte_idx += remaining;
                remaining = 0;
            }
        }

        // 解析chunk size（十六进制）
        size_t chunk_size = 0;
        auto parsed_chunk_size = parseChunkSizeLine(size_buffer);
        if (!parsed_chunk_size) {
            return std::unexpected(parsed_chunk_size.error());
        }
        chunk_size = *parsed_chunk_size;

        // 检查是否是最后一个chunk
        if (chunk_size == 0) {
            // 最后一个chunk，需要消费trailing \r\n
            std::string trailing_buffer;
            size_t trailing_consumed = 0;
            if (!findCRLF(iovecs, iov_idx, byte_idx, trailing_buffer, trailing_consumed)) {
                // trailing CRLF不完整
                if (has_parsed_chunk) {
                    return std::pair<bool, size_t>{false, total_consumed};
                }
                return std::unexpected(HttpError(kIncomplete));
            }

            if (!trailing_buffer.empty()) {
                // trailing应该是空的
                return std::unexpected(HttpError(kInvalidChunkFormat));
            }

            total_consumed += trailing_consumed;
            return std::pair<bool, size_t>{true, total_consumed};
        }

        // 计算剩余可用数据
        size_t available = 0;
        for (size_t i = iov_idx; i < iovecs.size(); ++i) {
            if (i == iov_idx) {
                available += iovecs[i].iov_len - byte_idx;
            } else {
                available += iovecs[i].iov_len;
            }
        }

        // 检查是否有足够的数据（chunk data + \r\n）
        if (chunk_size > std::numeric_limits<size_t>::max() - 2) {
            return std::unexpected(HttpError(kInvalidChunkLength));
        }
        if (available < chunk_size + 2) {
            // 数据不完整
            if (has_parsed_chunk) {
                return std::pair<bool, size_t>{false, total_consumed};
            }
            return std::unexpected(HttpError(kIncomplete));
        }

        // 读取chunk data
        size_t read_bytes = readData(iovecs, iov_idx, byte_idx, chunk_size, chunk_data);
        if (read_bytes != chunk_size) {
            return std::unexpected(HttpError(kInvalidChunkFormat));
        }

        total_consumed += chunk_size;

        // 更新位置
        remaining = chunk_size;
        while (remaining > 0 && iov_idx < iovecs.size()) {
            size_t available_in_iov = iovecs[iov_idx].iov_len - byte_idx;
            if (available_in_iov <= remaining) {
                remaining -= available_in_iov;
                iov_idx++;
                byte_idx = 0;
            } else {
                byte_idx += remaining;
                remaining = 0;
            }
        }

        // 验证并消费trailing \r\n
        if (iov_idx >= iovecs.size()) {
            return std::unexpected(HttpError(kInvalidChunkFormat));
        }

        const char* data = static_cast<const char*>(iovecs[iov_idx].iov_base);
        size_t len = iovecs[iov_idx].iov_len;

        if (byte_idx + 2 <= len) {
            if (data[byte_idx] != '\r' || data[byte_idx + 1] != '\n') {
                return std::unexpected(HttpError(kInvalidChunkFormat));
            }
            byte_idx += 2;
        } else if (byte_idx + 1 == len && data[byte_idx] == '\r') {
            if (iov_idx + 1 >= iovecs.size()) {
                return std::unexpected(HttpError(kInvalidChunkFormat));
            }
            const char* next_data = static_cast<const char*>(iovecs[iov_idx + 1].iov_base);
            if (next_data[0] != '\n') {
                return std::unexpected(HttpError(kInvalidChunkFormat));
            }
            iov_idx++;
            byte_idx = 1;
        } else {
            return std::unexpected(HttpError(kInvalidChunkFormat));
        }

        total_consumed += 2;
        has_parsed_chunk = true;

        // 继续循环，尝试解析更多chunk
    }

    // 如果解析了至少一个chunk，返回false表示还有更多chunk
    if (has_parsed_chunk) {
        return std::pair<bool, size_t>{false, total_consumed};
    }

    // 没有解析到完整的chunk
    return std::unexpected(HttpError(kIncomplete));
}

std::expected<std::pair<bool, size_t>, HttpError>
ChunkParser::parse(const std::vector<iovec>& iovecs,
                   std::string& chunk_data,
                   size_t max_body_size)
{
    size_t consumed = 0;

    for (const auto& iov : iovecs) {
        const char* data = static_cast<const char*>(iov.iov_base);
        size_t pos = 0;

        while (pos < iov.iov_len) {
            if (m_phase == Phase::kDone) {
                return std::pair<bool, size_t>{true, consumed};
            }

            if (m_phase == Phase::kSizeLine) {
                const char c = data[pos++];
                ++consumed;

                if (m_pending_cr) {
                    m_pending_cr = false;
                    if (c != '\n') {
                        return std::unexpected(HttpError(kInvalidChunkFormat));
                    }

                    auto parsed_chunk_size = parseChunkSizeLine(m_line_buffer);
                    if (!parsed_chunk_size) {
                        return std::unexpected(parsed_chunk_size.error());
                    }

                    m_line_buffer.clear();
                    m_current_chunk_size = *parsed_chunk_size;
                    m_current_chunk_read = 0;

                    if (m_current_chunk_size == 0) {
                        m_phase = Phase::kLastTrailerLine;
                    } else {
                        if (wouldExceedBodyLimit(chunk_data.size(), m_current_chunk_size, max_body_size)) {
                            return std::unexpected(HttpError(
                                kRequestEntityTooLarge,
                                "chunked body exceeds max body size"));
                        }
                        m_phase = Phase::kData;
                    }
                    continue;
                }

                if (c == '\r') {
                    m_pending_cr = true;
                    continue;
                }
                if (c == '\n') {
                    return std::unexpected(HttpError(kInvalidChunkFormat));
                }
                if (m_line_buffer.size() >= kMaxChunkLineSize) {
                    return std::unexpected(HttpError(kInvalidChunkLength));
                }
                m_line_buffer.push_back(c);
                continue;
            }

            if (m_phase == Phase::kData) {
                const size_t chunk_remaining = m_current_chunk_size - m_current_chunk_read;
                const size_t input_remaining = iov.iov_len - pos;
                const size_t to_read = std::min(chunk_remaining, input_remaining);
                if (to_read > 0) {
                    chunk_data.append(data + pos, to_read);
                    pos += to_read;
                    consumed += to_read;
                    m_current_chunk_read += to_read;
                }
                if (m_current_chunk_read == m_current_chunk_size) {
                    m_phase = Phase::kDataCrlf;
                    m_pending_cr = false;
                }
                continue;
            }

            if (m_phase == Phase::kDataCrlf) {
                const char c = data[pos++];
                ++consumed;

                if (!m_pending_cr) {
                    if (c != '\r') {
                        return std::unexpected(HttpError(kInvalidChunkFormat));
                    }
                    m_pending_cr = true;
                    continue;
                }

                m_pending_cr = false;
                if (c != '\n') {
                    return std::unexpected(HttpError(kInvalidChunkFormat));
                }

                m_current_chunk_size = 0;
                m_current_chunk_read = 0;
                m_phase = Phase::kSizeLine;
                continue;
            }

            if (m_phase == Phase::kLastTrailerLine) {
                const char c = data[pos++];
                ++consumed;

                if (m_pending_cr) {
                    m_pending_cr = false;
                    if (c != '\n') {
                        return std::unexpected(HttpError(kInvalidChunkFormat));
                    }
                    if (!m_line_buffer.empty()) {
                        return std::unexpected(HttpError(kInvalidChunkFormat));
                    }
                    m_phase = Phase::kDone;
                    return std::pair<bool, size_t>{true, consumed};
                }

                if (c == '\r') {
                    m_pending_cr = true;
                    continue;
                }
                if (c == '\n') {
                    return std::unexpected(HttpError(kInvalidChunkFormat));
                }
                if (m_line_buffer.size() >= kMaxChunkLineSize) {
                    return std::unexpected(HttpError(kInvalidChunkLength));
                }
                m_line_buffer.push_back(c);
                continue;
            }
        }
    }

    return std::pair<bool, size_t>{m_phase == Phase::kDone, consumed};
}

void ChunkParser::reset()
{
    m_phase = Phase::kSizeLine;
    m_line_buffer.clear();
    m_current_chunk_size = 0;
    m_current_chunk_read = 0;
    m_pending_cr = false;
}

std::string Chunk::toChunk(const std::string& data, bool is_last)
{
    return toChunk(data.data(), data.size(), is_last);
}

std::string Chunk::toChunk(const char* data, size_t length, bool is_last)
{
    if (is_last) {
        return "0\r\n\r\n";
    }

    std::ostringstream oss;
    oss << std::hex << length << "\r\n";
    std::string result = oss.str();
    result.append(data, length);
    result.append("\r\n");
    return result;
}

bool Chunk::findCRLF(const std::vector<iovec>& iovecs,
                     size_t start_iov,
                     size_t start_byte,
                     std::string& buffer,
                     size_t& consumed)
{
    buffer.clear();
    consumed = 0;
    size_t iov_idx = start_iov;
    size_t byte_idx = start_byte;

    while (iov_idx < iovecs.size()) {
        const char* data = static_cast<const char*>(iovecs[iov_idx].iov_base);
        size_t len = iovecs[iov_idx].iov_len;
        size_t i = byte_idx;

#if defined(GALAY_HTTP_SIMD_NEON)
        // ARM NEON 优化：一次扫描 16 字节查找 \r
        if (len - i >= 16) {
            uint8x16_t cr_vec = vdupq_n_u8('\r');

            for (; i + 16 <= len; i += 16) {
                uint8x16_t chunk = vld1q_u8(reinterpret_cast<const uint8_t*>(data + i));
                uint8x16_t cmp = vceqq_u8(chunk, cr_vec);

                // 检查是否有匹配
                uint64x2_t result = vreinterpretq_u64_u8(cmp);
                uint64_t low = vgetq_lane_u64(result, 0);
                uint64_t high = vgetq_lane_u64(result, 1);

                if (low != 0 || high != 0) {
                    // 找到 \r，回退到标量处理
                    break;
                }

                // 没有 \r，批量添加到 buffer
                buffer.append(data + i, 16);
                consumed += 16;
            }
        }
#elif defined(GALAY_HTTP_SIMD_X86)
        // x86 SSE2 优化：一次扫描 16 字节查找 \r
        if (len - i >= 16) {
            __m128i cr_vec = _mm_set1_epi8('\r');

            for (; i + 16 <= len; i += 16) {
                __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(data + i));
                __m128i cmp = _mm_cmpeq_epi8(chunk, cr_vec);
                int mask = _mm_movemask_epi8(cmp);

                if (mask != 0) {
                    // 找到 \r，回退到标量处理
                    break;
                }

                // 没有 \r，批量添加到 buffer
                buffer.append(data + i, 16);
                consumed += 16;
            }
        }
#endif

        // 标量处理剩余字节和 \r\n 验证
        for (; i < len; ++i) {
            char c = data[i];
            consumed++;

            if (c == '\r') {
                // 检查下一个字符是否是\n
                if (i + 1 < len) {
                    if (data[i + 1] == '\n') {
                        consumed++;
                        return true;
                    }
                } else if (iov_idx + 1 < iovecs.size()) {
                    // \n在下一个iovec中
                    const char* next_data = static_cast<const char*>(iovecs[iov_idx + 1].iov_base);
                    if (next_data[0] == '\n') {
                        consumed++;
                        return true;
                    }
                } else {
                    // 数据不完整
                    return false;
                }
            } else if (c == '\n') {
                // 错误：没有\r直接\n
                return false;
            } else {
                buffer.push_back(c);
            }
        }

        iov_idx++;
        byte_idx = 0;
    }

    return false;
}

size_t Chunk::readData(const std::vector<iovec>& iovecs,
                       size_t start_iov,
                       size_t start_byte,
                       size_t length,
                       std::string& output)
{
    size_t read_bytes = 0;
    size_t iov_idx = start_iov;
    size_t byte_idx = start_byte;

    while (read_bytes < length && iov_idx < iovecs.size()) {
        const char* data = static_cast<const char*>(iovecs[iov_idx].iov_base);
        size_t len = iovecs[iov_idx].iov_len;
        size_t available = len - byte_idx;
        size_t to_read = std::min(available, length - read_bytes);

        output.append(data + byte_idx, to_read);
        read_bytes += to_read;

        if (to_read == available) {
            iov_idx++;
            byte_idx = 0;
        } else {
            byte_idx += to_read;
        }
    }

    return read_bytes;
}

} // namespace galay::http
