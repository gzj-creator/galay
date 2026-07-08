/**
 * @file http_conn.h
 * @brief HTTP 连接封装
 * @author galay-http
 * @version 1.0.0
 *
 * @details 封装 HTTP/HTTPS 连接的底层 Socket 与 RingBuffer 资源，
 *          提供 Reader/Writer 工厂方法，支持向 WebSocket/HTTP2 协议升级。
 */

#ifndef GALAY_HTTP_CONN_H
#define GALAY_HTTP_CONN_H

#include "http_reader.h"
#include "http_writer.h"
#include "../../galay-kernel/async/tcp_socket.h"
#include "../../galay-utils/cache/ring_buffer.hpp"

namespace galay::websocket {
    template<typename SocketType>
    class WsConnImpl;  // 前向声明
}

namespace galay::http2 {
    template<typename SocketType, ::galay::utils::RingBufferBackendStrategy Strategy>
    class Http2ConnImpl;  // 前向声明
}

namespace galay::http
{

using namespace galay::async;
using namespace galay::kernel;
using ::galay::utils::RingBuffer;

/**
 * @brief HTTP连接模板类
 * @tparam SocketType Socket类型（TcpSocket 或 SslSocket）
 * @details 封装HTTP连接的底层资源和配置，不处理业务逻辑
 */
template<typename SocketType>
class HttpConnImpl
{
public:
    /**
     * @brief 默认每连接 RingBuffer 大小。
     * @details 作为单次 socket 读取窗口使用；HTTP reader 会跨多轮 I/O 继续推进，
     *          因此该值不是请求体大小上限。
     */
    static constexpr size_t kDefaultRingBufferSize = 64 * 1024;

    /**
     * @brief 构造函数
     * @param socket Socket右值引用
     */
    HttpConnImpl(SocketType&& socket)
        : m_socket(std::move(socket))
        , m_ring_buffer(kDefaultRingBufferSize)
    {
    }

    HttpConnImpl(SocketType&& socket, RingBuffer<>&& ring_buffer)
        : m_socket(std::move(socket))
        , m_ring_buffer(std::move(ring_buffer))
    {
    }

    ~HttpConnImpl() = default;

    // 禁用拷贝
    HttpConnImpl(const HttpConnImpl&) = delete;
    HttpConnImpl& operator=(const HttpConnImpl&) = delete;

    // 启用移动
    HttpConnImpl(HttpConnImpl&&) = default;
    HttpConnImpl& operator=(HttpConnImpl&&) = default;

    /**
     * @brief 关闭连接
     * @return CloseAwaitable 关闭等待体
     */
    auto close() {
        return m_socket.close();
    }

    /**
     * @brief SSL shutdown（仅对 SslSocket 有效）
     * @return ShutdownAwaitable shutdown等待体
     */
    auto shutdown() {
        return m_socket.shutdown();
    }

    /**
     * @brief 获取HttpReader
     * @param setting HttpReaderSetting配置
     * @return HttpReaderImpl<SocketType> Reader对象
     */
    HttpReaderImpl<SocketType> getReader(const HttpReaderSetting& setting = HttpReaderSetting()) {
        return HttpReaderImpl<SocketType>(m_ring_buffer, setting, m_socket);
    }

    /**
     * @brief 获取HttpWriter
     * @param setting HttpWriterSetting配置
     * @return HttpWriterImpl<SocketType> Writer对象
     */
    HttpWriterImpl<SocketType> getWriter() {
        return HttpWriterImpl<SocketType>(m_default_writer_setting, m_socket);
    }

    /**
     * @brief 获取HttpWriter
     * @param setting HttpWriterSetting配置
     * @return HttpWriterImpl<SocketType> Writer对象
     */
    HttpWriterImpl<SocketType> getWriter(const HttpWriterSetting& setting) {
        return HttpWriterImpl<SocketType>(setting, m_socket);
    }

    /**
     * @brief 设置连接级默认 HttpWriter 配置
     * @param setting 默认写入器配置
     */
    void setDefaultWriterSetting(HttpWriterSetting setting) {
        m_default_writer_setting = std::move(setting);
    }

    /**
     * @brief 获取连接级默认 HttpWriter 配置
     * @return 默认写入器配置引用
     */
    const HttpWriterSetting& defaultWriterSetting() const {
        return m_default_writer_setting;
    }

    /**
     * @brief 获取底层 Socket 引用
     * @return SocketType 引用
     * @note 用于需要直接访问底层 socket 的场景（如 WebSocket 升级后的处理）
     */
    SocketType& getSocket() { return m_socket; }

    // 允许HttpServerImpl访问私有成员
    template<typename S>
    friend class HttpServerImpl;

    // 允许HttpRouterImpl访问私有成员
    friend class HttpRouter;

    // 允许WsConnImpl访问私有成员（用于从HttpConnImpl构造）
    template<typename S>
    friend class galay::websocket::WsConnImpl;

    // 允许Http2ConnImpl访问私有成员（用于h2c升级）
    template<typename S, ::galay::utils::RingBufferBackendStrategy Strategy>
    friend class galay::http2::Http2ConnImpl;

private:
    /**
     * @brief 获取底层socket（私有方法，仅供友元类使用）
     * @return SocketType引用
     */
    SocketType& socket() { return m_socket; }

    /**
     * @brief 获取RingBuffer（私有方法，仅供友元类使用）
     * @return RingBuffer引用
     */
    RingBuffer<>& ringBuffer() { return m_ring_buffer; }

    SocketType m_socket;
    RingBuffer<> m_ring_buffer;
    HttpWriterSetting m_default_writer_setting;
};

// 类型别名 - HTTP (TcpSocket)
using HttpConn = HttpConnImpl<TcpSocket>;

} // namespace galay::http

#ifdef GALAY_SSL_FEATURE_ENABLED
#include "../../galay-ssl/async/ssl_socket.h"
namespace galay::http {
using HttpsConn = HttpConnImpl<galay::ssl::SslSocket>;
} // namespace galay::http
#endif

#endif // GALAY_HTTP_CONN_H
