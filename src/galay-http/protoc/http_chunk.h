/**
 * @file http_chunk.h
 * @brief HTTP 分块传输编码（Chunked Transfer Encoding）处理
 * @author galay-http
 * @version 1.0.0
 *
 * @details 提供 HTTP Chunked 编码的解析与生成功能，支持从 iovec
 *          离散缓冲区中增量解析 chunk 数据。
 */

#ifndef GALAY_HTTP_CHUNK_H
#define GALAY_HTTP_CHUNK_H

#include "http_error.h"
#include <string>
#include <vector>
#include <sys/uio.h>
#include <expected>

namespace galay::http
{

/**
 * @brief HTTP Chunk编码处理类
 * @details 提供chunk编码的解析和创建功能
 */
class Chunk
{
public:
    /**
     * @brief 从iovec中解析chunk数据
     * @param iovecs 输入的iovec数组
     * @param chunk_data 输出的chunk数据（追加方式）
     * @return std::expected<std::pair<bool, size_t>, HttpError>
     *         - pair.first: true表示读取到最后一个chunk，false表示还有更多chunk
     *         - pair.second: 消费的字节数
     *         - HttpError: 解析错误或数据不完整
     * @details 每次调用尝试解析一个或多个完整的chunk
     *          数据不完整时返回kIncomplete错误
     */
    static std::expected<std::pair<bool, size_t>, HttpError>
    fromIOVec(const std::vector<iovec>& iovecs, std::string& chunk_data);

    /**
     * @brief 创建chunk编码的数据
     * @param data 要编码的数据
     * @param is_last 是否是最后一个chunk
     * @return std::string chunk编码后的字符串
     * @details 格式：size(hex)\r\ndata\r\n
     *          最后一个chunk：0\r\n\r\n
     */
    static std::string toChunk(const std::string& data, bool is_last = false);

    /**
     * @brief 创建chunk编码的数据（从buffer）
     * @param data 要编码的数据指针
     * @param length 数据长度
     * @param is_last 是否是最后一个chunk
     * @return std::string chunk编码后的字符串
     */
    static std::string toChunk(const char* data, size_t length, bool is_last = false);

private:
    /**
     * @brief 从iovec中查找\r\n
     * @param iovecs iovec数组
     * @param start_iov 起始iovec索引
     * @param start_byte 起始字节索引
     * @param buffer 用于存储\r\n之前的数据
     * @param consumed 输出消费的字节数
     * @return true找到\r\n，false未找到
     */
    static bool findCRLF(const std::vector<iovec>& iovecs,
                        size_t start_iov,
                        size_t start_byte,
                        std::string& buffer,
                        size_t& consumed);

    /**
     * @brief 从iovec中读取指定长度的数据
     * @param iovecs iovec数组
     * @param start_iov 起始iovec索引
     * @param start_byte 起始字节索引
     * @param length 要读取的长度
     * @param output 输出buffer
     * @return 实际读取的字节数
     */
    static size_t readData(const std::vector<iovec>& iovecs,
                          size_t start_iov,
                          size_t start_byte,
                          size_t length,
                          std::string& output);

    /**
     * @brief 将数字转换为十六进制字符串
     * @param value 数值
     * @return 十六进制字符串
     */
    static std::string toHex(size_t value);
};

/**
 * @brief 有状态的 HTTP chunked body 增量解析器
 * @details 支持单个 chunk 跨多轮 RingBuffer 读取，解析到 chunk size 后会先检查
 *          max_body_size，避免超大 chunk 在完整到达前占满接收窗口。max_body_size
 *          为 0 表示不限制 body 大小。
 */
class ChunkParser
{
public:
    /**
     * @brief 从 iovec 增量解析 chunked body
     * @param iovecs 输入的 iovec 数组
     * @param chunk_data 输出 body，解析到的 chunk payload 会追加到该字符串
     * @param max_body_size 最大 body 字节数，0 表示无限制
     * @return pair.first 为是否读到最后一个 chunk，pair.second 为本次消耗字节数
     */
    std::expected<std::pair<bool, size_t>, HttpError>
    parse(const std::vector<iovec>& iovecs,
          std::string& chunk_data,
          size_t max_body_size = 0);

    /**
     * @brief 重置解析状态，用于新的 chunked body
     */
    void reset();

private:
    enum class Phase
    {
        kSizeLine,
        kData,
        kDataCrlf,
        kLastTrailerLine,
        kDone,
    };

    Phase m_phase = Phase::kSizeLine;       ///< 当前解析阶段
    std::string m_line_buffer;              ///< chunk size 行或 trailer 行缓存
    size_t m_current_chunk_size = 0;        ///< 当前 chunk payload 总长度
    size_t m_current_chunk_read = 0;        ///< 当前 chunk 已读取 payload 长度
    bool m_pending_cr = false;              ///< 是否刚读取到 '\r'
};

} // namespace galay::http

#endif // GALAY_HTTP_CHUNK_H
