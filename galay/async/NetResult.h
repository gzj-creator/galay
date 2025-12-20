#ifndef GALAY_NET_RESULT_H
#define GALAY_NET_RESULT_H

#include "galay/common/Base.h"
#include "galay/common/Common.h"
#include "galay/common/Bytes.h"
#include "galay/kernel/Engine.h"
#include "galay/kernel/Waker.h"
#include "galay/kernel/Coroutine.hpp"
#include <variant>

namespace galay::details {

class AsyncTcpSocketBuilder;

// 网络事件类型枚举
enum class NetEventType {
    Accept,
    Recv,
    Send,
    Connect,
    Close,
    Recvfrom,
    Sendto
};

// Accept参数
struct AcceptParams {
    GHandle* accept_handle = nullptr;
    GHandle listen_handle;
    Engine* engine = nullptr;
};

// Recv参数
struct RecvParams {
    char* buffer = nullptr;
    size_t length = 0;
    GHandle socket_handle;
    Engine* engine = nullptr;
};

// Send参数
struct SendParams {
    Bytes bytes;
    GHandle socket_handle;
    Engine* engine = nullptr;
};

// Connect参数
struct ConnectParams {
    Host host;
    GHandle socket_handle;
    Engine* engine = nullptr;
};

// Close参数
struct CloseParams {
    GHandle handle;
    Engine* engine = nullptr;
};

// Recvfrom参数
struct RecvfromParams {
    Host* remote = nullptr;
    char* buffer = nullptr;
    size_t length = 0;
    GHandle socket_handle;
    Engine* engine = nullptr;
};

// Sendto参数
struct SendtoParams {
    Host remote;
    Bytes bytes;
    GHandle socket_handle;
    Engine* engine = nullptr;
};

// 网络事件参数联合体
using NetEventParams = std::variant<
    AcceptParams,
    RecvParams,
    SendParams,
    ConnectParams,
    CloseParams,
    RecvfromParams,
    SendtoParams
>;

// 网络等待体类
template<typename ResultType>
class NetResult
{
public:
    using ptr = std::shared_ptr<NetResult>;
    using wptr = std::weak_ptr<NetResult>;

    NetResult(NetEventType type, NetEventParams params, WakerWrapper* wrapper);
    ~NetResult() = default;

    // 协程接口
    bool await_ready();
    bool await_suspend(std::coroutine_handle<> handle);
    std::expected<ResultType, CommonError> await_resume();

private:
    // 根据类型调用对应的ready检查
    bool checkAcceptReady();
    bool checkRecvReady();
    bool checkSendReady();
    bool checkConnectReady();
    bool checkCloseReady();
    bool checkRecvfromReady();
    bool checkSendtoReady();

    // 根据类型调用对应的suspend处理
    bool handleAcceptSuspend(Waker waker);
    bool handleRecvSuspend(Waker waker);
    bool handleSendSuspend(Waker waker);
    bool handleConnectSuspend(Waker waker);
    bool handleCloseSuspend(Waker waker);
    bool handleRecvfromSuspend(Waker waker);
    bool handleSendtoSuspend(Waker waker);

    // 根据类型调用对应的resume处理
    std::expected<ResultType, CommonError> getAcceptResult();
    std::expected<ResultType, CommonError> getRecvResult();
    std::expected<ResultType, CommonError> getSendResult();
    std::expected<ResultType, CommonError> getConnectResult();
    std::expected<ResultType, CommonError> getCloseResult();
    std::expected<ResultType, CommonError> getRecvfromResult();
    std::expected<ResultType, CommonError> getSendtoResult();

    // 内部实现方法
    bool acceptSocket();
    bool recvBytes();
    bool sendBytes();
    bool connectToHost();
    bool recvfromBytes();
    bool sendtoBytes();
    void closeSocket();

private:
    NetEventType m_type;
    NetEventParams m_params;
    WakerWrapper* m_wrapper;
    std::expected<ResultType, CommonError> m_result;

    // io_uring支持
    Waker m_waker;
    int m_io_result = 0;
    sockaddr_storage m_addr_storage;
    socklen_t m_addr_len_storage = 0;
};

// 网络事件类型别名
using AcceptNetResult = NetResult<AsyncTcpSocketBuilder>;
using RecvNetResult = NetResult<Bytes>;
using SendNetResult = NetResult<Bytes>;
using ConnectNetResult = NetResult<void>;
using CloseNetResult = NetResult<void>;
using RecvfromNetResult = NetResult<Bytes>;
using SendtoNetResult = NetResult<Bytes>;

} // namespace galay::details

#include "NetResult.inl"

#endif // GALAY_NET_RESULT_H