#ifndef GALAY_STATIC_FILE_CONFIG_H
#define GALAY_STATIC_FILE_CONFIG_H

#include <cstddef>

namespace galay::http
{

/**
 * @brief 静态文件传输模式
 */
enum class FileTransferMode
{
    /**
     * @brief 内存模式 - 将文件完整读入内存后发送
     * @details 适合小文件，简单高效，但占用内存
     */
    MEMORY,

    /**
     * @brief Chunk 模式 - 使用 HTTP chunked 编码分块传输
     * @details 适合中等文件，支持流式传输，内存占用可控
     */
    CHUNK,

    /**
     * @brief SendFile 模式 - 使用零拷贝 sendfile 系统调用
     * @details 适合大文件，性能最优，CPU 占用低，但不支持 chunked 编码
     */
    SENDFILE,

    /**
     * @brief 自动模式 - 根据文件大小自动选择传输方式
     * @details 小文件用 MEMORY，中等文件用 CHUNK，大文件用 SENDFILE
     */
    AUTO
};

/**
 * @brief 静态文件服务配置类
 * @details 用于配置静态文件的传输方式和性能参数
 */
class StaticFileConfig
{
public:
    /**
     * @brief 默认构造函数
     * @details 使用默认配置：
     *          - 传输模式：AUTO
     *          - 小文件阈值：64KB
     *          - 大文件阈值：1MB
     *          - Chunk 大小：64KB
     *          - ETag：启用
     */
    StaticFileConfig()
        : m_transfer_mode(FileTransferMode::AUTO)
        , m_small_file_threshold(64 * 1024)        // 64KB
        , m_large_file_threshold(1024 * 1024)      // 1MB
        , m_chunk_size(64 * 1024)                  // 64KB
        , m_sendfile_chunk_size(10 * 1024 * 1024) // 10MB
        , m_enable_cache(false)
        , m_enable_etag(true)
        , m_max_cache_size(100 * 1024 * 1024)     // 100MB
    {
    }

    /**
     * @brief 设置文件传输模式
     * @param mode 传输模式
     */
    void setTransferMode(FileTransferMode mode) {
        m_transfer_mode = mode;
    }

    /**
     * @brief 获取文件传输模式
     * @return 传输模式
     */
    FileTransferMode getTransferMode() const {
        return m_transfer_mode;
    }

    /**
     * @brief 设置小文件阈值（用于 AUTO 模式）
     * @param threshold 阈值（字节），小于此值使用 MEMORY 模式
     */
    void setSmallFileThreshold(size_t threshold) {
        m_small_file_threshold = threshold;
    }

    /**
     * @brief 获取小文件阈值
     * @return 阈值（字节）
     */
    size_t getSmallFileThreshold() const {
        return m_small_file_threshold;
    }

    /**
     * @brief 设置大文件阈值（用于 AUTO 模式）
     * @param threshold 阈值（字节），大于此值使用 SENDFILE 模式
     */
    void setLargeFileThreshold(size_t threshold) {
        m_large_file_threshold = threshold;
    }

    /**
     * @brief 获取大文件阈值
     * @return 阈值（字节）
     */
    size_t getLargeFileThreshold() const {
        return m_large_file_threshold;
    }

    /**
     * @brief 设置 Chunk 大小
     * @param size Chunk 大小（字节）
     */
    void setChunkSize(size_t size) {
        m_chunk_size = size;
    }

    /**
     * @brief 获取 Chunk 大小
     * @return Chunk 大小（字节）
     */
    size_t getChunkSize() const {
        return m_chunk_size;
    }

    /**
     * @brief 设置 SendFile 每次传输的块大小
     * @param size 块大小（字节）
     */
    void setSendFileChunkSize(size_t size) {
        m_sendfile_chunk_size = size;
    }

    /**
     * @brief 获取 SendFile 每次传输的块大小
     * @return 块大小（字节）
     */
    size_t getSendFileChunkSize() const {
        return m_sendfile_chunk_size;
    }

    /**
     * @brief 设置是否启用文件缓存
     * @param enable 是否启用
     * @note 仅对 mountHardly() 有效
     */
    void setEnableCache(bool enable) {
        m_enable_cache = enable;
    }

    /**
     * @brief 获取是否启用文件缓存
     * @return 是否启用
     */
    bool isEnableCache() const {
        return m_enable_cache;
    }

    /**
     * @brief 设置是否启用 ETag 条件请求
     * @param enable 是否启用
     * @details 启用后支持 If-None-Match / If-Match，并可返回 304
     */
    void setEnableETag(bool enable) {
        m_enable_etag = enable;
    }

    /**
     * @brief 获取是否启用 ETag 条件请求
     * @return 是否启用
     */
    bool isEnableETag() const {
        return m_enable_etag;
    }

    /**
     * @brief 设置最大缓存大小
     * @param size 最大缓存大小（字节）
     */
    void setMaxCacheSize(size_t size) {
        m_max_cache_size = size;
    }

    /**
     * @brief 获取最大缓存大小
     * @return 最大缓存大小（字节）
     */
    size_t getMaxCacheSize() const {
        return m_max_cache_size;
    }

    /**
     * @brief 根据文件大小决定传输模式（用于 AUTO 模式）
     * @param file_size 文件大小（字节）
     * @return 实际使用的传输模式
     */
    FileTransferMode decideTransferMode(size_t file_size) const {
        if (m_transfer_mode != FileTransferMode::AUTO) {
            return m_transfer_mode;
        }

        // AUTO 模式：根据文件大小自动选择
        if (file_size <= m_small_file_threshold) {
            return FileTransferMode::MEMORY;
        } else if (file_size <= m_large_file_threshold) {
            return FileTransferMode::CHUNK;
        } else {
            return FileTransferMode::SENDFILE;
        }
    }

private:
    FileTransferMode m_transfer_mode;    ///< 文件传输模式
    size_t m_small_file_threshold;       ///< 小文件阈值（字节）
    size_t m_large_file_threshold;       ///< 大文件阈值（字节）
    size_t m_chunk_size;                 ///< Chunk 大小（字节）
    size_t m_sendfile_chunk_size;        ///< SendFile 块大小（字节）
    bool m_enable_cache;                 ///< 是否启用缓存
    bool m_enable_etag;                  ///< 是否启用 ETag 条件请求
    size_t m_max_cache_size;             ///< 最大缓存大小（字节）
};

} // namespace galay::http

#endif // GALAY_STATIC_FILE_CONFIG_H
