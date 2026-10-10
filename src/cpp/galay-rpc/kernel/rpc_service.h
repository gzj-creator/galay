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
 *         register_method("echo", &EchoService::echo);
 *     }
 *
 *     Task<void> echo(RpcContext& ctx) {
 *         auto& req = ctx.request();
 *         ctx.response().payload(req.payload().data(), req.payload().size());
 *         co_return;
 *     }
 * };
 * @endcode
 */

#ifndef GALAY_RPC_SERVICE_H
#define GALAY_RPC_SERVICE_H

#include "../protoc/rpc_message.h"
#include "../protoc/rpc_error.h"
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
using RpcMethodHandler = std::function<Task<void>(RpcContext&)>;
/// @brief RPC流处理函数类型
using RpcStreamHandler = std::function<Task<void>(RpcStream&)>;

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

private:
    RpcService(const RpcService&) = delete;
    RpcService& operator=(const RpcService&) = delete;
public:
    RpcService(RpcService&&) = delete;
    RpcService& operator=(RpcService&&) = delete;

    /**
     * @brief 获取服务名称
     * @return const std::string& 只读引用
     */
    const std::string& name() const { return m_name; }

    /**
     * @brief 查找方法处理器
     * @param method 方法名
     * @return 方法处理器，未找到返回nullptr
     */
    RpcMethodHandler* find_method(const std::string& method) {
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
    RpcMethodHandler* find_method(const std::string& method, RpcCallMode mode) {
        if (mode == RpcCallMode::UNARY) {
            return find_method(method);
        }

        auto it = m_stream_methods.find(method);
        if (it == m_stream_methods.end()) {
            return nullptr;
        }

        const size_t mode_idx = stream_mode_index(mode);
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
    RpcStreamHandler* find_stream_method(const std::string& method) {
        auto it = m_stream_session_methods.find(method);
        if (it != m_stream_session_methods.end()) {
            return &it->second;
        }
        return nullptr;
    }

    /**
     * @brief 获取所有方法名
     * @return 处理后的 std::vector<std::string> 结果
     */
    std::vector<std::string> method_names() const {
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
     * @param name 名称
     * @param handler 处理回调
     * @return 无返回值
     */
    void register_method(std::string_view name, RpcMethodHandler handler) {
        register_unary_method(name, std::move(handler));
    }

    /**
     * @brief 注册一元成员方法（兼容旧接口）
     * @tparam T 服务类型
     * @param name 方法名
     * @param method 成员函数指针
     * @return 无返回值
     */
    template<typename T>
    void register_method(std::string_view name, Task<void> (T::*method)(RpcContext&)) {
        register_unary_method(name, method);
    }

    /// @brief 注册一元方法
    /// @param name 名称
    /// @param handler 处理回调
    /// @return 无返回值
    void register_unary_method(std::string_view name, RpcMethodHandler handler) {
        register_method_by_mode(name, RpcCallMode::UNARY, std::move(handler));
    }

    /// @brief 注册客户端流方法
    /// @param name 名称
    /// @param handler 处理回调
    /// @return 无返回值
    void register_client_streaming_method(std::string_view name, RpcMethodHandler handler) {
        register_method_by_mode(name, RpcCallMode::CLIENT_STREAMING, std::move(handler));
    }

    /// @brief 注册服务端流方法
    /// @param name 名称
    /// @param handler 处理回调
    /// @return 无返回值
    void register_server_streaming_method(std::string_view name, RpcMethodHandler handler) {
        register_method_by_mode(name, RpcCallMode::SERVER_STREAMING, std::move(handler));
    }

    /// @brief 注册双向流方法
    /// @param name 名称
    /// @param handler 处理回调
    /// @return 无返回值
    void register_bidi_streaming_method(std::string_view name, RpcMethodHandler handler) {
        register_method_by_mode(name, RpcCallMode::BIDI_STREAMING, std::move(handler));
    }

    /// @brief 注册一元成员方法
    /// @param name 名称
    /// @param method 服务成员方法指针
    /// @return 无返回值
    template<typename T>
    void register_unary_method(std::string_view name, Task<void> (T::*method)(RpcContext&)) {
        register_member_method(name, RpcCallMode::UNARY, method);
    }

    /// @brief 注册客户端流成员方法
    /// @param name 名称
    /// @param method 服务成员方法指针
    /// @return 无返回值
    template<typename T>
    void register_client_streaming_method(std::string_view name, Task<void> (T::*method)(RpcContext&)) {
        register_member_method(name, RpcCallMode::CLIENT_STREAMING, method);
    }

    /// @brief 注册服务端流成员方法
    /// @param name 名称
    /// @param method 服务成员方法指针
    /// @return 无返回值
    template<typename T>
    void register_server_streaming_method(std::string_view name, Task<void> (T::*method)(RpcContext&)) {
        register_member_method(name, RpcCallMode::SERVER_STREAMING, method);
    }

    /// @brief 注册双向流成员方法
    /// @param name 名称
    /// @param method 服务成员方法指针
    /// @return 无返回值
    template<typename T>
    void register_bidi_streaming_method(std::string_view name, Task<void> (T::*method)(RpcContext&)) {
        register_member_method(name, RpcCallMode::BIDI_STREAMING, method);
    }

    /**
     * @brief 注册流会话方法（独立于帧级流模式）
     * @param name 方法名
     * @param handler 流处理函数
     * @return 无返回值
     */
    void register_stream_method(std::string_view name, RpcStreamHandler handler) {
        m_stream_session_methods[std::string(name)] = std::move(handler);
    }

    /**
     * @brief 注册流会话成员方法
     * @tparam T 服务类型
     * @param name 方法名
     * @param method 成员函数指针
     * @return 无返回值
     */
    template<typename T>
    void register_stream_method(std::string_view name, Task<void> (T::*method)(RpcStream&)) {
        m_stream_session_methods[std::string(name)] =
            [this, method](RpcStream& stream) -> Task<void> {
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
    /// @param mode 操作模式
    /// @return 流模式索引；客户端流为 0、服务端流为 1、双向流为 2，其他模式为 0
    static size_t stream_mode_index(RpcCallMode mode) {
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
    /// @param name 名称
    /// @param mode 操作模式
    /// @param handler 处理回调
    /// @return 无返回值
    void register_method_by_mode(std::string_view name, RpcCallMode mode, RpcMethodHandler handler) {
        const std::string method_name(name);
        if (mode == RpcCallMode::UNARY) {
            m_unary_methods[method_name] = std::move(handler);
            return;
        }

        auto& slots = m_stream_methods[method_name];
        const size_t mode_idx = stream_mode_index(mode);
        slots.handlers[mode_idx] = std::move(handler);
        slots.registered[mode_idx] = true;
    }

    /// @brief 注册成员方法（将成员函数包装为RpcMethodHandler）
    /// @param name 名称
    /// @param mode 操作模式
    /// @param method 服务成员方法指针
    /// @return 无返回值
    template<typename T>
    void register_member_method(std::string_view name,
                              RpcCallMode mode,
                              Task<void> (T::*method)(RpcContext&)) {
        register_method_by_mode(name,
                             mode,
                             [this, method](RpcContext& ctx) -> Task<void> {
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
     * @return RpcRequest& 引用
     */
    RpcRequest& request() { return m_request; }
    const RpcRequest& request() const { return m_request; }

    /**
     * @brief 获取响应
     * @return RpcResponse& 引用
     */
    RpcResponse& response() { return m_response; }
    const RpcResponse& response() const { return m_response; }

    /**
     * @brief 设置错误
     * @param code 错误码
     * @return 无返回值
     */
    void set_error(RpcErrorCode code) {
        m_response.error_code(code);
    }

    /**
     * @brief 设置响应数据
     * @param data 输入数据
     * @param len 数据字节数
     * @return 无返回值
     */
    void set_payload(const char* data, size_t len) {
        m_response.payload(data, len);
    }

    /// @brief 设置响应数据（字符串）
    /// @param data 输入数据引用
    /// @return 无返回值
    void set_payload(const std::string& data) {
        m_response.payload(data.data(), data.size());
    }

    /// @brief 设置响应数据（移动向量）
    /// @param data 输入数据引用
    /// @return 无返回值
    void set_payload(std::vector<char>&& data) {
        m_response.payload(std::move(data));
    }

    /**
     * @brief 设置响应payload视图（零拷贝借用模式）
     * @param view 外部payload视图
     * @return 无返回值
     * @note 需确保view引用的数据在响应发送完成前有效
     */
    void set_payload(const RpcPayloadView& view) {
        m_response.payload_view(view);
    }

private:
    RpcRequest& m_request;    ///< 请求引用
    RpcResponse& m_response;  ///< 响应引用
};

} // namespace galay::rpc

#endif // GALAY_RPC_SERVICE_H
