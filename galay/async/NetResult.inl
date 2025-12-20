#ifndef GALAY_NETRESULT_INL
#define GALAY_NETRESULT_INL

#include "NetResult.h"
#include "galay/kernel/Waker.h"
#include "galay/kernel/Coroutine.hpp"
#include "galay/common/Log.h"
#include "galay/common/Error.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <variant>

// macOS上MSG_NOSIGNAL不可用，定义为0
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace galay::details {

// NetResult构造函数
template<typename ResultType>
NetResult<ResultType>::NetResult(NetEventType type, NetEventParams params, WakerWrapper* wrapper)
    : m_type(type), m_params(std::move(params)), m_wrapper(wrapper)
{
}

// await_ready实现
template<typename ResultType>
bool NetResult<ResultType>::await_ready()
{
    switch (m_type) {
        case NetEventType::Accept:
            return checkAcceptReady();
        case NetEventType::Recv:
            return checkRecvReady();
        case NetEventType::Send:
            return checkSendReady();
        case NetEventType::Connect:
            return checkConnectReady();
        case NetEventType::Close:
            return checkCloseReady();
        case NetEventType::Recvfrom:
            return checkRecvfromReady();
        case NetEventType::Sendto:
            return checkSendtoReady();
        default:
            return true;
    }
}

// await_suspend实现
template<typename ResultType>
bool NetResult<ResultType>::await_suspend(std::coroutine_handle<> handle)
{
    auto co = std::coroutine_handle<PromiseTypeBase>::from_address(handle.address()).promise().getCoroutine();
    Waker waker(co);

    switch (m_type) {
        case NetEventType::Accept:
            return handleAcceptSuspend(waker);
        case NetEventType::Recv:
            return handleRecvSuspend(waker);
        case NetEventType::Send:
            return handleSendSuspend(waker);
        case NetEventType::Connect:
            return handleConnectSuspend(waker);
        case NetEventType::Close:
            return handleCloseSuspend(waker);
        case NetEventType::Recvfrom:
            return handleRecvfromSuspend(waker);
        case NetEventType::Sendto:
            return handleSendtoSuspend(waker);
    }
    return false;
}

// await_resume实现
template<typename ResultType>
std::expected<ResultType, CommonError> NetResult<ResultType>::await_resume()
{
    switch (m_type) {
        case NetEventType::Accept:
            return getAcceptResult();
        case NetEventType::Recv:
            return getRecvResult();
        case NetEventType::Send:
            return getSendResult();
        case NetEventType::Connect:
            return getConnectResult();
        case NetEventType::Close:
            return getCloseResult();
        case NetEventType::Recvfrom:
            return getRecvfromResult();
        case NetEventType::Sendto:
            return getSendtoResult();
        default:
            return std::unexpected(CommonError(1, 0));
    }
}

// Accept相关实现
template<typename ResultType>
bool NetResult<ResultType>::checkAcceptReady()
{
    // 尝试非阻塞accept
    return acceptSocket();
}

template<typename ResultType>
bool NetResult<ResultType>::handleAcceptSuspend(Waker waker)
{
#if defined(USE_IOURING)
    using namespace error;
    auto* params = std::get_if<AcceptParams>(&m_params);
    if (!params || !params->engine) {
        m_result = std::unexpected(CommonError(CallActiveEventError, 1));
        return false;
    }

    // io_uring模式：提交accept操作
    auto& addr = m_addr_storage;
    auto& addr_len = m_addr_len_storage;
    addr_len = sizeof(addr);

    // 这里需要Engine有submitAccept方法
    // if (!params->engine->submitAccept(this, params->listen_handle.fd, &addr, &addr_len)) {
    //     m_result = std::unexpected(CommonError(CallActiveEventError, static_cast<uint32_t>(errno)));
    //     return false;
    // }
    m_waker = waker;
    return true;
#else
    // Reactor模式：注册读事件
    auto* params = std::get_if<AcceptParams>(&m_params);
    if (!params || !params->engine) {
        m_result = std::unexpected(CommonError(1, static_cast<uint32_t>(errno)));
        return false;
    }
    params->engine->addWakers(m_wrapper, WakerType::READ, std::move(waker), params->listen_handle, nullptr);
    return true;
#endif
}

template<typename ResultType>
std::expected<ResultType, CommonError> NetResult<ResultType>::getAcceptResult()
{
    if constexpr (std::is_same_v<ResultType, void>) {
        if (m_result) {
            return {};
        } else {
            return std::move(m_result);
        }
    } else {
        return std::move(m_result);
    }
}

template<typename ResultType>
bool NetResult<ResultType>::acceptSocket()
{
    using namespace error;
    auto* params = std::get_if<AcceptParams>(&m_params);
    if (!params) {
        m_result = std::unexpected(CommonError(CallAcceptError, 1));
        return true;
    }

#if defined(USE_IOURING)
    // io_uring模式：在onResume中处理结果
    if (m_io_result >= 0) {
        params->accept_handle->fd = m_io_result;
        std::string ip = inet_ntoa(reinterpret_cast<sockaddr_in*>(&m_addr_storage)->sin_addr);
        uint16_t port = ntohs(reinterpret_cast<sockaddr_in*>(&m_addr_storage)->sin_port);
        LogTrace("[Accept Address: {}:{}]", ip, port);
        m_result = {};
    } else {
        m_result = std::unexpected(CommonError(CallAcceptError, static_cast<uint32_t>(-m_io_result)));
    }
    return true;
#else
    // Reactor模式：直接尝试accept
    sockaddr addr{};
    socklen_t addr_len = sizeof(addr);
    GHandle handle {
        .fd = accept(params->listen_handle.fd, &addr, &addr_len),
    };

    if( handle.fd < 0 ) {
        if( static_cast<uint32_t>(errno) == EAGAIN || static_cast<uint32_t>(errno) == EWOULDBLOCK || static_cast<uint32_t>(errno) == EINTR ) {
            return false;
        }
        m_result = std::unexpected(CommonError(CallAcceptError, static_cast<uint32_t>(errno)));
        return true;
    }

    std::string ip = inet_ntoa(reinterpret_cast<sockaddr_in*>(&addr)->sin_addr);
    uint16_t port = ntohs(reinterpret_cast<sockaddr_in*>(&addr)->sin_port);
    LogTrace("[Accept Address: {}:{}]", ip, port);
    m_result = {};
    *params->accept_handle = handle;
    return true;
#endif
}

// Recv相关实现
template<typename ResultType>
bool NetResult<ResultType>::checkRecvReady()
{
    return recvBytes();
}

template<typename ResultType>
bool NetResult<ResultType>::handleRecvSuspend(Waker waker)
{
#if defined(USE_IOURING)
    using namespace error;
    auto* params = std::get_if<RecvParams>(&m_params);
    if (!params || !params->engine) {
        m_result = std::unexpected(CommonError(CallActiveEventError, 1));
        return false;
    }

    // io_uring模式：提交recv操作
    // if (!params->engine->submitRecv(this, params->socket_handle.fd, params->buffer, params->length, 0)) {
    //     m_result = std::unexpected(CommonError(CallActiveEventError, static_cast<uint32_t>(errno)));
    //     return false;
    // }
    m_waker = waker;
    return true;
#else
    // Reactor模式：注册读事件
    auto* params = std::get_if<RecvParams>(&m_params);
    if (!params || !params->engine) {
        m_result = std::unexpected(CommonError(1, static_cast<uint32_t>(errno)));
        return false;
    }
    params->engine->addWakers(m_wrapper, WakerType::READ, std::move(waker), params->socket_handle, nullptr);
    return true;
#endif
}

template<typename ResultType>
std::expected<ResultType, CommonError> NetResult<ResultType>::getRecvResult()
{
    return std::move(m_result);
}

template<typename ResultType>
bool NetResult<ResultType>::recvBytes()
{
    using namespace error;
    auto* params = std::get_if<RecvParams>(&m_params);
    if (!params || !params->buffer) {
        m_result = std::unexpected(CommonError(CallRecvError, 1));
        return true;
    }

#if defined(USE_IOURING)
    // io_uring模式：在onResume中处理结果
    LogInfo("[NetResult::recvBytes] fd: {}, io_result: {}", params->socket_handle.fd, m_io_result);
    if (m_io_result > 0) {
        LogTrace("recvBytes: {}, buffer: {}", m_io_result, std::string(params->buffer, m_io_result));
        if constexpr (std::is_same_v<ResultType, Bytes>) {
            Bytes bytes = Bytes::fromCString(params->buffer, m_io_result, m_io_result);
            m_result = std::move(bytes);
        } else {
            m_result = {};
        }
    } else if (m_io_result == 0) {
        LogInfo("[NetResult::recvBytes] Connection closed by peer, fd: {}", params->socket_handle.fd);
        m_result = std::unexpected(CommonError(DisConnectError, 0));
    } else {
        LogError("[NetResult::recvBytes] recv error, fd: {}, result: {}", params->socket_handle.fd, m_io_result);
        m_result = std::unexpected(CommonError(CallRecvError, static_cast<uint32_t>(-m_io_result)));
    }
    return true;
#else
    // Reactor模式：直接尝试recv
    ssize_t recvBytes = recv(params->socket_handle.fd, params->buffer, params->length, 0);
    LogInfo("[NetResult::recvBytes] fd: {}, recvBytes: {}, errno: {}", params->socket_handle.fd, recvBytes, errno);

    if (recvBytes > 0) {
        LogTrace("recvBytes: {}, buffer: {}", recvBytes, std::string(params->buffer, recvBytes));
        if constexpr (std::is_same_v<ResultType, Bytes>) {
            Bytes bytes = Bytes::fromCString(params->buffer, recvBytes, recvBytes);
            m_result = std::move(bytes);
        } else {
            m_result = {};
        }
        return true;
    } else if (recvBytes == 0) {
        LogInfo("[NetResult::recvBytes] Connection closed by peer, fd: {}", params->socket_handle.fd);
        m_result = std::unexpected(CommonError(DisConnectError, static_cast<uint32_t>(errno)));
        return true;
    } else {
        if (static_cast<uint32_t>(errno) == EAGAIN || static_cast<uint32_t>(errno) == EWOULDBLOCK || static_cast<uint32_t>(errno) == EINTR) {
            LogInfo("[NetResult::recvBytes] Would block, fd: {}, errno: {}", params->socket_handle.fd, errno);
            return false;
        }
        LogError("[NetResult::recvBytes] recv error, fd: {}, errno: {}", params->socket_handle.fd, errno);
        m_result = std::unexpected(CommonError(CallRecvError, static_cast<uint32_t>(errno)));
        return true;
    }
#endif
}

// Send相关实现 - 类似模式
template<typename ResultType>
bool NetResult<ResultType>::checkSendReady()
{
    return sendBytes();
}

template<typename ResultType>
bool NetResult<ResultType>::handleSendSuspend(Waker waker)
{
#if defined(USE_IOURING)
    using namespace error;
    auto* params = std::get_if<SendParams>(&m_params);
    if (!params || !params->engine) {
        m_result = std::unexpected(CommonError(CallActiveEventError, 1));
        return false;
    }

    // io_uring模式：提交send操作
    // if (!params->engine->submitSend(this, params->socket_handle.fd, params->bytes.data(), params->bytes.size(), MSG_NOSIGNAL)) {
    //     m_result = std::unexpected(CommonError(CallActiveEventError, static_cast<uint32_t>(errno)));
    //     return false;
    // }
    m_waker = waker;
    return true;
#else
    // Reactor模式：注册写事件
    auto* params = std::get_if<SendParams>(&m_params);
    if (!params || !params->engine) {
        m_result = std::unexpected(CommonError(1, static_cast<uint32_t>(errno)));
        return false;
    }
    params->engine->addWakers(m_wrapper, WakerType::WRITE, std::move(waker), params->socket_handle, nullptr);
    return true;
#endif
}

template<typename ResultType>
std::expected<ResultType, CommonError> NetResult<ResultType>::getSendResult()
{
    return std::move(m_result);
}

template<typename ResultType>
bool NetResult<ResultType>::sendBytes()
{
    using namespace error;
    auto* params = std::get_if<SendParams>(&m_params);
    if (!params) {
        m_result = std::unexpected(CommonError(CallSendError, 1));
        return true;
    }

#if defined(USE_IOURING)
    // io_uring模式：在onResume中处理结果
    if (m_io_result > 0) {
        LogTrace("sendBytes: {}, buffer: {}", m_io_result, std::string(reinterpret_cast<const char*>(params->bytes.data())));
        if constexpr (std::is_same_v<ResultType, Bytes>) {
            Bytes remain(params->bytes.data() + m_io_result, params->bytes.size() - m_io_result);
            m_result = std::move(remain);
        } else {
            m_result = {};
        }
    } else if (m_io_result == 0) {
        if constexpr (std::is_same_v<ResultType, Bytes>) {
            m_result = Bytes{};
        } else {
            m_result = {};
        }
    } else {
        int err = -m_io_result;
        if (err == EPIPE || err == ECONNRESET) {
            m_result = std::unexpected(CommonError(DisConnectError, static_cast<uint32_t>(err)));
        } else {
            m_result = std::unexpected(CommonError(CallSendError, static_cast<uint32_t>(err)));
        }
    }
    return true;
#else
    // Reactor模式：直接尝试send
    // macOS上MSG_NOSIGNAL不可用，定义为0
    #ifndef MSG_NOSIGNAL
    #define MSG_NOSIGNAL 0
    #endif

    ssize_t sendBytes = send(params->socket_handle.fd, params->bytes.data(), params->bytes.size(), MSG_NOSIGNAL);
    if (sendBytes > 0) {
        LogTrace("sendBytes: {}, buffer: {}", sendBytes, std::string(reinterpret_cast<const char*>(params->bytes.data())));
        if constexpr (std::is_same_v<ResultType, Bytes>) {
            Bytes remain(params->bytes.data() + sendBytes, params->bytes.size() - sendBytes);
            m_result = std::move(remain);
        } else {
            m_result = {};
        }
        return true;
    } else if (sendBytes == 0) {
        if constexpr (std::is_same_v<ResultType, Bytes>) {
            m_result = Bytes{};
        } else {
            m_result = {};
        }
        return true;
    } else {
        if (static_cast<uint32_t>(errno) == EAGAIN || static_cast<uint32_t>(errno) == EWOULDBLOCK || static_cast<uint32_t>(errno) == EINTR) {
            return false;
        } else if (static_cast<uint32_t>(errno) == EPIPE || static_cast<uint32_t>(errno) == ECONNRESET) {
            m_result = std::unexpected(CommonError(DisConnectError, static_cast<uint32_t>(errno)));
            return true;
        }
        m_result = std::unexpected(CommonError(CallSendError, static_cast<uint32_t>(errno)));
        return true;
    }
#endif
}

// 其他事件的简化实现
template<typename ResultType>
bool NetResult<ResultType>::checkConnectReady() { return false; }
template<typename ResultType>
bool NetResult<ResultType>::checkCloseReady() { return false; }
template<typename ResultType>
bool NetResult<ResultType>::checkRecvfromReady() { return false; }
template<typename ResultType>
bool NetResult<ResultType>::checkSendtoReady() { return false; }

template<typename ResultType>
bool NetResult<ResultType>::handleConnectSuspend(Waker waker)
{
#if defined(USE_IOURING)
    using namespace error;
    auto* params = std::get_if<ConnectParams>(&m_params);
    if (!params || !params->engine) {
        m_result = std::unexpected(CommonError(CallActiveEventError, 1));
        return false;
    }

    // io_uring模式：提交connect操作
    m_addr_storage.ss_family = AF_INET;
    reinterpret_cast<sockaddr_in*>(&m_addr_storage)->sin_addr.s_addr = inet_addr(params->host.ip.c_str());
    reinterpret_cast<sockaddr_in*>(&m_addr_storage)->sin_port = htons(params->host.port);

    // if (!params->engine->submitConnect(this, params->socket_handle.fd, reinterpret_cast<sockaddr*>(&m_addr_storage), sizeof(m_addr_storage))) {
    //     m_result = std::unexpected(CommonError(CallActiveEventError, static_cast<uint32_t>(errno)));
    //     return false;
    // }
    m_waker = waker;
    return true;
#else
    // Reactor模式：注册写事件
    auto* params = std::get_if<ConnectParams>(&m_params);
    if (!params || !params->engine) {
        m_result = std::unexpected(CommonError(1, static_cast<uint32_t>(errno)));
        return false;
    }
    params->engine->addWakers(m_wrapper, WakerType::WRITE, std::move(waker), params->socket_handle, nullptr);
    return true;
#endif
}

template<typename ResultType>
bool NetResult<ResultType>::handleCloseSuspend(Waker waker)
{
#if defined(USE_IOURING)
    using namespace error;
    auto* params = std::get_if<CloseParams>(&m_params);
    if (!params || !params->engine) {
        m_result = std::unexpected(CommonError(CallActiveEventError, 1));
        return false;
    }

    // io_uring模式：提交close操作
    // if (!params->engine->submitClose(this, params->handle.fd)) {
    //     m_result = std::unexpected(CommonError(CallActiveEventError, static_cast<uint32_t>(errno)));
    //     return false;
    // }
    m_waker = waker;
    return true;
#else
    // Reactor模式：直接关闭
    auto* params = std::get_if<CloseParams>(&m_params);
    if (!params) {
        m_result = std::unexpected(CommonError(1, static_cast<uint32_t>(errno)));
        return false;
    }
    params->engine->delWakers(m_wrapper, WakerType::READ, Waker(), params->handle, nullptr);
    if (::close(params->handle.fd)) {
        m_result = std::unexpected(CommonError(static_cast<uint32_t>(errno), static_cast<uint32_t>(errno)));
    } else {
        m_result = {};
    }
    return true;
#endif
}

template<typename ResultType>
bool NetResult<ResultType>::handleRecvfromSuspend(Waker waker)
{
#if defined(USE_IOURING)
    using namespace error;
    auto* params = std::get_if<RecvfromParams>(&m_params);
    if (!params || !params->engine) {
        m_result = std::unexpected(CommonError(CallActiveEventError, 1));
        return false;
    }

    // io_uring模式：提交recvfrom操作
    // if (!params->engine->submitRecvfrom(this, params->socket_handle.fd, params->buffer, params->length, 0, &m_addr_storage, &m_addr_len_storage)) {
    //     m_result = std::unexpected(CommonError(CallActiveEventError, static_cast<uint32_t>(errno)));
    //     return false;
    // }
    m_waker = waker;
    return true;
#else
    // Reactor模式：注册读事件
    auto* params = std::get_if<RecvfromParams>(&m_params);
    if (!params || !params->engine) {
        m_result = std::unexpected(CommonError(1, static_cast<uint32_t>(errno)));
        return false;
    }
    params->engine->addWakers(m_wrapper, WakerType::READ, std::move(waker), params->socket_handle, nullptr);
    return true;
#endif
}

template<typename ResultType>
bool NetResult<ResultType>::handleSendtoSuspend(Waker waker)
{
#if defined(USE_IOURING)
    using namespace error;
    auto* params = std::get_if<SendtoParams>(&m_params);
    if (!params || !params->engine) {
        m_result = std::unexpected(CommonError(CallActiveEventError, 1));
        return false;
    }

    // io_uring模式：提交sendto操作
    m_addr_storage.ss_family = AF_INET;
    reinterpret_cast<sockaddr_in*>(&m_addr_storage)->sin_addr.s_addr = inet_addr(params->remote.ip.c_str());
    reinterpret_cast<sockaddr_in*>(&m_addr_storage)->sin_port = htons(params->remote.port);

    // if (!params->engine->submitSendto(this, params->socket_handle.fd, params->bytes.data(), params->bytes.size(), MSG_NOSIGNAL,
    //                                 reinterpret_cast<sockaddr*>(&m_addr_storage), sizeof(m_addr_storage))) {
    //     m_result = std::unexpected(CommonError(CallActiveEventError, static_cast<uint32_t>(errno)));
    //     return false;
    // }
    m_waker = waker;
    return true;
#else
    // Reactor模式：注册写事件
    auto* params = std::get_if<SendtoParams>(&m_params);
    if (!params || !params->engine) {
        m_result = std::unexpected(CommonError(1, static_cast<uint32_t>(errno)));
        return false;
    }
    params->engine->addWakers(m_wrapper, WakerType::WRITE, std::move(waker), params->socket_handle, nullptr);
    return true;
#endif
}

template<typename ResultType>
std::expected<ResultType, CommonError> NetResult<ResultType>::getConnectResult() { return {}; }
template<typename ResultType>
std::expected<ResultType, CommonError> NetResult<ResultType>::getCloseResult() { return {}; }
template<typename ResultType>
std::expected<ResultType, CommonError> NetResult<ResultType>::getRecvfromResult() { return {}; }
template<typename ResultType>
std::expected<ResultType, CommonError> NetResult<ResultType>::getSendtoResult() { return {}; }

template<typename ResultType>
bool NetResult<ResultType>::connectToHost() { return false; }
template<typename ResultType>
void NetResult<ResultType>::closeSocket() {
    auto* params = std::get_if<CloseParams>(&m_params);
    if (params) {
        close(params->handle.fd);
    }
}
template<typename ResultType>
bool NetResult<ResultType>::recvfromBytes() { return false; }
template<typename ResultType>
bool NetResult<ResultType>::sendtoBytes() { return false; }

// 显式实例化常用的模板
template class NetResult<void>;
template class NetResult<Bytes>;

} // namespace galay::details



#endif