#ifndef GALAY_HTTP_READER_SETTING_H
#define GALAY_HTTP_READER_SETTING_H

#include "../protoc/http_base.h"
#include <cstddef>

namespace galay::http
{

/**
 * @brief HTTP读取器配置类
 * @details 用于配置HttpReader的各种限制参数
 */
class HttpReaderSetting
{
public:
    HttpReaderSetting() = default;
    ~HttpReaderSetting() = default;

    /**
     * @brief 设置最大头部长度
     * @param max_header_size 最大头部长度（字节）
     */
    void setMaxHeaderSize(size_t max_header_size) {
        m_max_header_size = max_header_size;
    }

    /**
     * @brief 获取最大头部长度
     * @return 最大头部长度（字节）
     */
    size_t getMaxHeaderSize() const {
        return m_max_header_size;
    }

    /**
     * @brief 设置最大Body长度
     * @param max_body_size 最大Body长度（字节）
     */
    void setMaxBodySize(size_t max_body_size) {
        m_max_body_size = max_body_size;
    }

    /**
     * @brief 获取最大Body长度
     * @return 最大Body长度（字节）
     */
    size_t getMaxBodySize() const {
        return m_max_body_size;
    }

    /**
     * @brief 设置接收超时时间
     * @param timeout_ms 超时时间（毫秒）
     */
    void setRecvTimeout(int timeout_ms) {
        m_recv_timeout_ms = timeout_ms;
    }

    /**
     * @brief 获取接收超时时间
     * @return 超时时间（毫秒）
     */
    int getRecvTimeout() const {
        return m_recv_timeout_ms;
    }

private:
    size_t m_max_header_size = DEFAULT_HTTP_MAX_HEADER_SIZE;
    size_t m_max_body_size = DEFAULT_HTTP_MAX_BODY_SIZE;
    int m_recv_timeout_ms = DEFAULT_HTTP_RECV_TIME_MS;
};

} // namespace galay::http

#endif // GALAY_HTTP_READER_SETTING_H
