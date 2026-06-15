/**
 * @file rpc_service.h
 * @brief RPC服务定义
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details 提供RPC服务的基类和方法注册机制。
 *
 * @example
 * @code
 * class EchoService : public RpcService {
 * public:
 *     EchoService() : RpcService("EchoService") {
 *         registerMethod("echo", &EchoService::echo);
 *     }
 *
 *     Coroutine echo(RpcContext& ctx) {
 *         auto& req = ctx.request();
 *         ctx.response().payload(req.payload().data(), req.payload().size());
 *         co_return;
 *     }
 * };
 * @endcode
 */

#ifndef GALAY_RPC_SERVICE_H
#define GALAY_RPC_SERVICE_H

#include "galay-rpc/protoc/rpc_message.h"
#include "galay-rpc/protoc/rpc_error.h"
#include "rpc_stream.h"
#include <array>
#include <string>
#include <unordered_map>
#include <functional>
#include <memory>

namespace galay::rpc
{

class RpcContext;

/**
 * @brief RPC方法处理函数类型
 */
/// @brief RPC方法处理函数类型
using RpcMethodHandler = std::function<Coroutine(RpcContext&)>;
/// @brief RPC流处理函数类型
using RpcStreamHandler = std::function<Coroutine(RpcStream&)>;

/**
 * @brief RPC服务基类
 *
 * @details 提供RPC服务的基类和方法注册机制，支持一元调用、客户端流、
 *          服务端流和双向流四种调用模式，以及独立的流会话模式。
 */
class RpcService {
public:
    static constexpr size_t kStreamRpcModeCount = 3;  ///< 流式RPC模式数量

    /**
     * @brief 构造函数
     * @param name 服务名称
     */
    explicit RpcService(std::string_view name)
        : m_name(name) {}

    virtual ~RpcService() = default;

    /**
     * @brief 获取服务名称
     */
    const std::string& name() const { return m_name; }

    /**
     * @brief 查找方法处理器
     * @param method 方法名
     * @return 方法处理器，未找到返回nullptr
     */
    RpcMethodHandler* findMethod(const std::string& method) {
        auto it = m_unary_methods.find(method);
        if (it != m_unary_methods.end()) {
            return &it->second;
        }
        return nullptr;
    }

    /**
     * @brief 查找方法处理器（带调用模式）
     * @param method 方法名
     * @param mode 调用模式
     * @return 方法处理器，未找到返回nullptr
     */
    RpcMethodHandler* findMethod(const std::string& method, RpcCallMode mode) {
        if (mode == RpcCallMode::UNARY) {
            return findMethod(method);
        }

        auto it = m_stream_methods.find(method);
        if (it == m_stream_methods.end()) {
            return nullptr;
        }

        const size_t mode_idx = streamModeIndex(mode);
        if (!it->second.registered[mode_idx]) {
            return nullptr;
        }
        return &it->second.handlers[mode_idx];
    }

    /**
     * @brief 查找流会话处理器
     * @param method 方法名
     * @return 流处理器，未找到返回nullptr
     */
    RpcStreamHandler* findStreamMethod(const std::string& method) {
        auto it = m_stream_session_methods.find(method);
        if (it != m_stream_session_methods.end()) {
            return &it->second;
        }
        return nullptr;
    }

    /**
     * @brief 获取所有方法名
     */
    std::vector<std::string> methodNames() const {
        std::vector<std::string> names;
        names.reserve(m_unary_methods.size() + m_stream_methods.size());
        for (const auto& [name, _] : m_unary_methods) {
            names.push_back(name);
        }
        for (const auto& [name, _] : m_stream_methods) {
            if (m_unary_methods.find(name) == m_unary_methods.end()) {
                names.push_back(name);
            }
        }
        for (const auto& [name, _] : m_stream_session_methods) {
            if (m_unary_methods.find(name) == m_unary_methods.end() &&
                m_stream_methods.find(name) == m_stream_methods.end()) {
                names.push_back(name);
            }
        }
        return names;
    }

protected:
    /**
     * @brief 注册一元方法（兼容旧接口）
     */
    void registerMethod(std::string_view name, RpcMethodHandler handler) {
        registerUnaryMethod(name, std::move(handler));
    }

    /**
     * @brief 注册一元成员方法（兼容旧接口）
     * @tparam T 服务类型
     * @param name 方法名
     * @param method 成员函数指针
     */
    template<typename T>
    void registerMethod(std::string_view name, Coroutine (T::*method)(RpcContext&)) {
        registerUnaryMethod(name, method);
    }

    /// @brief 注册一元方法
    void registerUnaryMethod(std::string_view name, RpcMethodHandler handler) {
        registerMethodByMode(name, RpcCallMode::UNARY, std::move(handler));
    }

    /// @brief 注册客户端流方法
    void registerClientStreamingMethod(std::string_view name, RpcMethodHandler handler) {
        registerMethodByMode(name, RpcCallMode::CLIENT_STREAMING, std::move(handler));
    }

    /// @brief 注册服务端流方法
    void registerServerStreamingMethod(std::string_view name, RpcMethodHandler handler) {
        registerMethodByMode(name, RpcCallMode::SERVER_STREAMING, std::move(handler));
    }

    /// @brief 注册双向流方法
    void registerBidiStreamingMethod(std::string_view name, RpcMethodHandler handler) {
        registerMethodByMode(name, RpcCallMode::BIDI_STREAMING, std::move(handler));
    }

    /// @brief 注册一元成员方法
    template<typename T>
    void registerUnaryMethod(std::string_view name, Coroutine (T::*method)(RpcContext&)) {
        registerMemberMethod(name, RpcCallMode::UNARY, method);
    }

    /// @brief 注册客户端流成员方法
    template<typename T>
    void registerClientStreamingMethod(std::string_view name, Coroutine (T::*method)(RpcContext&)) {
        registerMemberMethod(name, RpcCallMode::CLIENT_STREAMING, method);
    }

    /// @brief 注册服务端流成员方法
    template<typename T>
    void registerServerStreamingMethod(std::string_view name, Coroutine (T::*method)(RpcContext&)) {
        registerMemberMethod(name, RpcCallMode::SERVER_STREAMING, method);
    }

    /// @brief 注册双向流成员方法
    template<typename T>
    void registerBidiStreamingMethod(std::string_view name, Coroutine (T::*method)(RpcContext&)) {
        registerMemberMethod(name, RpcCallMode::BIDI_STREAMING, method);
    }

    /**
     * @brief 注册流会话方法（独立于帧级流模式）
     * @param name 方法名
     * @param handler 流处理函数
     */
    void registerStreamMethod(std::string_view name, RpcStreamHandler handler) {
        m_stream_session_methods[std::string(name)] = std::move(handler);
    }

    /**
     * @brief 注册流会话成员方法
     * @tparam T 服务类型
     * @param name 方法名
     * @param method 成员函数指针
     */
    template<typename T>
    void registerStreamMethod(std::string_view name, Coroutine (T::*method)(RpcStream&)) {
        m_stream_session_methods[std::string(name)] =
            [this, method](RpcStream& stream) -> Coroutine {
                return (static_cast<T*>(this)->*method)(stream);
            };
    }

private:
    /// @brief 流式RPC方法槽位（每种模式一个handler）
    struct RpcMethodSlots {
        std::array<RpcMethodHandler, kStreamRpcModeCount> handlers{};      ///< 各模式的处理函数
        std::array<bool, kStreamRpcModeCount> registered{false, false, false};  ///< 各模式是否已注册
    };

    /// @brief 将流调用模式转换为索引
    static size_t streamModeIndex(RpcCallMode mode) {
        switch (mode) {
            case RpcCallMode::CLIENT_STREAMING:
                return 0;
            case RpcCallMode::SERVER_STREAMING:
                return 1;
            case RpcCallMode::BIDI_STREAMING:
                return 2;
            default:
                return 0;
        }
    }

    /// @brief 按调用模式注册方法
    void registerMethodByMode(std::string_view name, RpcCallMode mode, RpcMethodHandler handler) {
        const std::string method_name(name);
        if (mode == RpcCallMode::UNARY) {
            m_unary_methods[method_name] = std::move(handler);
            return;
        }

        auto& slots = m_stream_methods[method_name];
        const size_t mode_idx = streamModeIndex(mode);
        slots.handlers[mode_idx] = std::move(handler);
        slots.registered[mode_idx] = true;
    }

    /// @brief 注册成员方法（将成员函数包装为RpcMethodHandler）
    template<typename T>
    void registerMemberMethod(std::string_view name,
                              RpcCallMode mode,
                              Coroutine (T::*method)(RpcContext&)) {
        registerMethodByMode(name,
                             mode,
                             [this, method](RpcContext& ctx) -> Coroutine {
                                 return (static_cast<T*>(this)->*method)(ctx);
                             });
    }

private:
    std::string m_name;                                             ///< 服务名
    std::unordered_map<std::string, RpcMethodHandler> m_unary_methods;      ///< 一元方法注册表
    std::unordered_map<std::string, RpcMethodSlots> m_stream_methods;       ///< 流式方法注册表
    std::unordered_map<std::string, RpcStreamHandler> m_stream_session_methods;  ///< 流会话方法注册表
};

/**
 * @brief RPC上下文
 *
 * @details 封装请求和响应，提供给服务方法使用。
 */
class RpcContext {
public:
    /**
     * @brief 构造RPC上下文
     * @param request 请求对象引用
     * @param response 响应对象引用
     */
    RpcContext(RpcRequest& request, RpcResponse& response)
        : m_request(request)
        , m_response(response) {}

    /**
     * @brief 获取请求
     */
    RpcRequest& request() { return m_request; }
    const RpcRequest& request() const { return m_request; }

    /**
     * @brief 获取响应
     */
    RpcResponse& response() { return m_response; }
    const RpcResponse& response() const { return m_response; }

    /**
     * @brief 设置错误
     */
    void setError(RpcErrorCode code) {
        m_response.errorCode(code);
    }

    /**
     * @brief 设置响应数据
     */
    void setPayload(const char* data, size_t len) {
        m_response.payload(data, len);
    }

    /// @brief 设置响应数据（字符串）
    void setPayload(const std::string& data) {
        m_response.payload(data.data(), data.size());
    }

    /// @brief 设置响应数据（移动向量）
    void setPayload(std::vector<char>&& data) {
        m_response.payload(std::move(data));
    }

    /**
     * @brief 设置响应payload视图（零拷贝借用模式）
     * @param view 外部payload视图
     * @note 需确保view引用的数据在响应发送完成前有效
     */
    void setPayload(const RpcPayloadView& view) {
        m_response.payloadView(view);
    }

private:
    RpcRequest& m_request;    ///< 请求引用
    RpcResponse& m_response;  ///< 响应引用
};

} // namespace galay::rpc

#endif // GALAY_RPC_SERVICE_H
