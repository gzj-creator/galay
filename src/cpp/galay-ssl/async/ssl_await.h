/**
 * @file ssl_await.h
 * @brief SSL 异步状态机与操作驱动器
 * @author galay-ssl
 * @version 1.0.0
 *
 * @details 提供 SSL 异步操作的核心基础设施：
 * - SslMachineSignal/SslMachineAction: 状态机信号与动作
 * - SslOperationDriver: 驱动 SSL IO 操作的底层引擎
 * - SslStateMachineAwaitable: 通用 SSL 状态机协程可等待对象
 * - SslAwaitableBuilder: 线性 SSL 操作流水线构建器
 * - SslLinearMachine: 线性多步骤 SSL 状态机
 *
 * @note 本文件是 galay-ssl 异步架构的核心，通常不直接使用，
 * 而是通过 SslSocket 的高层接口或 SslAwaitableBuilder 间接使用。
 */

#ifndef GALAY_SSL_AWAIT_H
#define GALAY_SSL_AWAIT_H

#include "../common/error.h"
#include "../../galay-utils/cache/bytes.hpp"
#include "../../galay-kernel/core/awaitable.h"
#include "../../galay-kernel/core/timeout.hpp"
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace galay::ssl
{

using namespace galay::kernel;
using ::galay::utils::Bytes;

class SslSocket;

/**
 * @brief SSL 状态机信号
 * @details 表示状态机当前步骤需要执行的 SSL 操作类型
 */
enum class SslMachineSignal : uint8_t {
    kContinue,     ///< 继续当前操作
    kHandshake,    ///< 执行握手
    kRecv,         ///< 执行接收
    kSend,         ///< 执行发送
    kShutdown,     ///< 执行关闭
    kComplete,     ///< 操作完成
    kFail,         ///< 操作失败
};

/**
 * @brief SSL 状态机动作
 * @tparam ResultT 操作结果类型
 * @details 描述状态机一步的输出：信号类型、IO 缓冲区和可选结果/错误
 */
template <typename ResultT>
struct SslMachineAction {
    SslMachineSignal signal = SslMachineSignal::kContinue;  ///< 当前信号
    char* read_buffer = nullptr;                             ///< 接收缓冲区指针
    size_t read_length = 0;                                  ///< 接收缓冲区大小
    const char* write_buffer = nullptr;                      ///< 发送缓冲区指针
    size_t write_length = 0;                                 ///< 发送数据长度
    std::optional<ResultT> result;                           ///< 操作结果（kComplete 时有效）
    std::optional<SslError> error;                           ///< 操作错误（kFail 时有效）

    /**
     * @brief 创建继续信号的动作
     * @return kContinue 动作
     */
    static SslMachineAction continue_()
    {
        return {};
    }

    /**
     * @brief 创建握手信号的动作
     * @return kHandshake 动作
     */
    static SslMachineAction handshake()
    {
        SslMachineAction action;
        action.signal = SslMachineSignal::kHandshake;
        return action;
    }

    /**
     * @brief 创建接收信号的动作
     * @param buffer 接收缓冲区
     * @param length 缓冲区大小
     * @return kRecv 动作
     */
    static SslMachineAction recv(char* buffer, size_t length)
    {
        SslMachineAction action;
        action.signal = SslMachineSignal::kRecv;
        action.read_buffer = buffer;
        action.read_length = length;
        return action;
    }

    /**
     * @brief 创建发送信号的动作
     * @param buffer 发送数据
     * @param length 数据长度
     * @return kSend 动作
     */
    static SslMachineAction send(const char* buffer, size_t length)
    {
        SslMachineAction action;
        action.signal = SslMachineSignal::kSend;
        action.write_buffer = buffer;
        action.write_length = length;
        return action;
    }

    /**
     * @brief 创建关闭信号的动作
     * @return kShutdown 动作
     */
    static SslMachineAction shutdown()
    {
        SslMachineAction action;
        action.signal = SslMachineSignal::kShutdown;
        return action;
    }

    /**
     * @brief 创建完成信号的动作
     * @param value 操作结果
     * @return kComplete 动作
     */
    static SslMachineAction complete(ResultT value)
    {
        SslMachineAction action;
        action.signal = SslMachineSignal::kComplete;
        action.result = std::move(value);
        return action;
    }

    /**
     * @brief 创建失败信号的动作
     * @param error 错误信息
     * @return kFail 动作
     */
    static SslMachineAction fail(SslError error)
    {
        SslMachineAction action;
        action.signal = SslMachineSignal::kFail;
        action.error = std::move(error);
        return action;
    }
};

/**
 * @brief SSL 可等待状态机概念
 * @tparam MachineT 状态机类型
 * @details 约束状态机必须提供 advance、onHandshake、onRecv、onSend、onShutdown 接口
 */
template <typename MachineT>
concept SslAwaitableStateMachine =
    requires(MachineT& machine,
             std::expected<void, SslError> handshake_result,
             std::expected<Bytes, SslError> recv_result,
             std::expected<size_t, SslError> send_result,
             std::expected<void, SslError> shutdown_result) {
        typename MachineT::result_type;
        { machine.advance() } -> std::same_as<SslMachineAction<typename MachineT::result_type>>;
        { machine.onHandshake(std::move(handshake_result)) } -> std::same_as<void>;
        { machine.onRecv(std::move(recv_result)) } -> std::same_as<void>;
        { machine.onSend(std::move(send_result)) } -> std::same_as<void>;
        { machine.onShutdown(std::move(shutdown_result)) } -> std::same_as<void>;
    };

/**
 * @brief SSL 握手上下文
 * @details 保存握手操作的结果
 */
struct SslHandshakeContext {
    std::expected<void, SslError> m_result{};  ///< 握手结果
};

/**
 * @brief SSL 接收上下文
 * @details 保存接收操作的缓冲区和结果
 */
struct SslRecvContext {
    /**
     * @brief 构造接收上下文
     * @param buffer 接收缓冲区指针
     * @param length 缓冲区大小
     */
    SslRecvContext(char* buffer = nullptr, size_t length = 0)
        : m_buffer(buffer)
        , m_length(length) {}

    char* m_buffer = nullptr;                      ///< 接收缓冲区指针
    size_t m_length = 0;                           ///< 缓冲区大小
    std::expected<Bytes, SslError> m_result{};     ///< 接收结果
};

/**
 * @brief SSL 发送上下文
 * @details 保存发送操作的缓冲区和结果
 */
struct SslSendContext {
    /**
     * @brief 构造发送上下文
     * @param buffer 发送数据指针
     * @param length 数据长度
     */
    SslSendContext(const char* buffer = nullptr, size_t length = 0)
        : m_buffer(buffer)
        , m_length(length) {}

    const char* m_buffer = nullptr;                ///< 发送数据指针
    size_t m_length = 0;                           ///< 数据长度
    std::expected<size_t, SslError> m_result{};    ///< 发送结果（已发送字节数）
};

/**
 * @brief SSL 关闭上下文
 * @details 保存关闭操作的结果
 */
struct SslShutdownContext {
    std::expected<void, SslError> m_result{};      ///< 关闭结果
};

/**
 * @brief SSL 构建器结果容器
 * @tparam ResultT 结果类型
 * @details 管理线性流水线中某一步的结果，支持设置、获取和重置
 */
template <typename ResultT>
class SslBuilderOutcome
{
public:
    /**
     * @brief 设置完成值
     * @tparam ValueT 可转换到 ResultT 的值类型
     * @param value 结果值
     */
    template <typename ValueT>
    void complete(ValueT&& value)
    {
        m_result = std::forward<ValueT>(value);
        m_result_set = true;
        m_queue_used = false;
    }

    /**
     * @brief 清除状态
     */
    void clear()
    {
        m_result.reset();
        m_result_set = false;
        m_queue_used = false;
    }

    /**
     * @brief 标记使用了队列操作
     */
    void markQueueUsed()
    {
        m_queue_used = true;
    }

    /**
     * @brief 检查是否有结果值
     * @return 是否已设置结果
     */
    bool hasResultValue() const
    {
        return m_result_set && m_result.has_value();
    }

    /**
     * @brief 取出结果值
     * @return 结果值，取出后内部状态重置
     */
    std::optional<ResultT> takeResultValue()
    {
        m_result_set = false;
        auto result = std::move(m_result);
        m_result.reset();
        return result;
    }

    /**
     * @brief 检查是否使用了队列操作
     * @return 是否使用了队列
     */
    bool queueUsed() const
    {
        return m_queue_used;
    }

    /**
     * @brief 重置（等同于 clear）
     */
    void reset()
    {
        clear();
    }

private:
    std::optional<ResultT> m_result;  ///< 结果值
    bool m_result_set = false;        ///< 结果是否已设置
    bool m_queue_used = false;        ///< 是否使用了队列操作
};

/**
 * @brief SSL 构建器操作辅助类
 * @tparam ResultT 结果类型
 * @tparam InlineN 内联步骤容量
 * @details 提供线性流水线步骤中的操作接口，如队列管理和结果设置
 */
template <typename ResultT, size_t InlineN = 4>
class SslBuilderOps
{
public:
    /**
     * @brief 构造操作辅助对象
     * @param owner 所属的结果容器
     */
    explicit SslBuilderOps(SslBuilderOutcome<ResultT>& owner)
        : m_owner(owner) {}

    /**
     * @brief 将步骤加入队列
     * @tparam StepT 步骤类型
     * @param step 步骤引用
     * @return 步骤引用
     */
    template <typename StepT>
    StepT& queue(StepT& step)
    {
        m_owner.markQueueUsed();
        return step;
    }

    /**
     * @brief 将多个步骤批量加入队列
     * @tparam StepTs 步骤类型包
     * @param steps 步骤引用包
     */
    template <typename... StepTs>
    void queueMany(StepTs&... steps)
    {
        (queue(steps), ...);
    }

    /**
     * @brief 清除所属结果容器
     */
    void clear()
    {
        m_owner.clear();
    }

    /**
     * @brief 设置完成值
     * @tparam ValueT 值类型
     * @param value 结果值
     */
    template <typename ValueT>
    void complete(ValueT&& value)
    {
        m_owner.complete(std::forward<ValueT>(value));
    }

private:
    SslBuilderOutcome<ResultT>& m_owner;  ///< 所属结果容器
};

/**
 * @brief 内部辅助类型
 */
namespace detail {

/**
 * @brief 判断类型是否为 std::expected 的类型特征
 * @tparam T 待判断类型
 */
template <typename T>
struct is_expected : std::false_type {};

template <typename T, typename E>
struct is_expected<std::expected<T, E>> : std::true_type {};

/**
 * @brief is_expected 的便捷变量模板
 * @tparam ResultT 待判断类型
 */
template <typename ResultT>
constexpr bool is_expected_v = is_expected<std::remove_cvref_t<ResultT>>::value;

/**
 * @brief 提取 std::expected 的 value_type 和 error_type
 * @tparam ResultT expected 类型
 */
template <typename ResultT>
struct expected_traits;

template <typename T, typename E>
struct expected_traits<std::expected<T, E>> {
    using value_type = T;   ///< 值类型
    using error_type = E;   ///< 错误类型
};

} // namespace detail

/**
 * @brief SSL IO 操作驱动器
 * @details 驱动单个 SSL 操作（握手/接收/发送/关闭）的底层引擎，
 * 管理 SSL 引擎与网络 IO 之间的数据流转。
 */
class SslOperationDriver
{
public:
    /**
     * @brief 等待类型
     */
    enum class WaitKind : uint8_t {
        kNone,   ///< 无需等待
        kRead,   ///< 等待读取
        kWrite,  ///< 等待写入
    };

    /**
     * @brief 等待动作
     */
    struct WaitAction {
        WaitKind kind = WaitKind::kNone;  ///< 等待类型
        IOContextBase* context = nullptr;  ///< IO 上下文
    };

    /**
     * @brief 构造操作驱动器
     * @param socket SSL Socket 指针
     */
    explicit SslOperationDriver(SslSocket* socket);

    /**
     * @brief 启动握手操作
     */
    void startHandshake();

    /**
     * @brief 启动接收操作
     * @param buffer 接收缓冲区
     * @param length 缓冲区大小
     */
    void startRecv(char* buffer, size_t length);

    /**
     * @brief 启动发送操作
     * @param buffer 发送数据
     * @param length 数据长度
     */
    void startSend(const char* buffer, size_t length);

    /**
     * @brief 启动关闭操作
     */
    void startShutdown();

    /**
     * @brief 轮询驱动器状态
     * @return 等待动作，指示下一步需要等待的 IO 类型
     */
    WaitAction poll();

    /**
     * @brief 处理读取完成事件
     * @param result 读取结果
     */
    void onRead(std::expected<size_t, IOError> result);

    /**
     * @brief 处理写入完成事件
     * @param result 写入结果
     */
    void onWrite(std::expected<size_t, IOError> result);

    /**
     * @brief 检查操作是否已完成
     * @return 是否完成
     */
    bool completed() const;

    /**
     * @brief 取出握手结果
     * @return 握手结果
     */
    std::expected<void, SslError> takeHandshakeResult();

    /**
     * @brief 取出接收结果
     * @return 接收结果（包含字节数据）
     */
    std::expected<Bytes, SslError> takeRecvResult();

    /**
     * @brief 取出发送结果
     * @return 发送结果（已发送字节数）
     */
    std::expected<size_t, SslError> takeSendResult();

    /**
     * @brief 取出关闭结果
     * @return 关闭结果
     */
    std::expected<void, SslError> takeShutdownResult();

    /**
     * @brief 获取接收 IO 上下文
     * @return 接收上下文引用
     */
    RecvIOContext& recvContext() { return m_recv_context; }

    /**
     * @brief 获取发送 IO 上下文
     * @return 发送上下文引用
     */
    SendIOContext& sendContext() { return m_send_context; }

private:
    /**
     * @brief 操作类型
     */
    enum class OperationKind : uint8_t {
        kNone,       ///< 无操作
        kHandshake,  ///< 握手
        kRecv,       ///< 接收
        kSend,       ///< 发送
        kShutdown,   ///< 关闭
    };

    /**
     * @brief 接收轮询动作
     */
    enum class RecvPollAction : uint8_t {
        kNeedRecv,   ///< 需要接收
        kNeedSend,   ///< 需要发送
        kCompleted,  ///< 已完成
    };

    void resetContexts();                                    ///< 重置所有 IO 上下文
    void resetHandshakeState();                              ///< 重置握手状态
    void resetRecvState();                                   ///< 重置接收状态
    void resetSendState();                                   ///< 重置发送状态
    void resetShutdownState();                               ///< 重置关闭状态
    void clearOperation();                                   ///< 清除当前操作

    WaitAction pollHandshake();                              ///< 轮询握手进度
    WaitAction pollRecv();                                   ///< 轮询接收进度
    WaitAction pollSend();                                   ///< 轮询发送进度
    WaitAction pollShutdown();                               ///< 轮询关闭进度

    void onHandshakeRead(std::expected<size_t, IOError> result);   ///< 处理握手读取完成
    void onHandshakeWrite(std::expected<size_t, IOError> result);  ///< 处理握手写入完成
    void onRecvRead(std::expected<size_t, IOError> result);        ///< 处理接收读取完成
    void onRecvWrite(std::expected<size_t, IOError> result);       ///< 处理接收写入完成
    void onSendRead(std::expected<size_t, IOError> result);        ///< 处理发送读取完成
    void onSendWrite(std::expected<size_t, IOError> result);       ///< 处理发送写入完成
    void onShutdownRead(std::expected<size_t, IOError> result);    ///< 处理关闭读取完成
    void onShutdownWrite(std::expected<size_t, IOError> result);   ///< 处理关闭写入完成

    bool prepareReadBuffer(std::vector<char>& buffer);                    ///< 准备读取缓冲区
    bool prepareWriteFromPending(std::vector<char>& buffer, size_t pending, SslErrorCode error_code);  ///< 从待发送数据准备写入缓冲区
    bool prepareRecvSendChunk(size_t pending);                            ///< 准备接收发送块
    bool fillSendChunk(size_t pending = 0);                                ///< 填充发送块
    RecvPollAction drainRecvPlaintext();                                  ///< 排空接收明文

    void setHandshakeFailure(SslError error);   ///< 设置握手失败
    void setRecvFailure(SslError error);        ///< 设置接收失败
    void setSendFailure(SslError error);        ///< 设置发送失败
    void setShutdownSuccess();                  ///< 设置关闭成功
    void clearTransientBuffers();               ///< 清除临时缓冲区

    OperationKind m_operation = OperationKind::kNone;  ///< 当前操作类型
    SslSocket* m_socket = nullptr;                     ///< SSL Socket 指针
    RecvIOContext m_recv_context;                       ///< 接收 IO 上下文
    SendIOContext m_send_context;                       ///< 发送 IO 上下文

    /**
     * @brief 握手状态
     */
    struct HandshakeState {
        bool result_set = false;                         ///< 结果是否已设置
        std::expected<void, SslError> result{};          ///< 握手结果
        bool flush_success = false;                      ///< 是否已成功刷新
        bool wait_read_after_write = false;              ///< 写入后是否等待读取
        bool read_pending = false;                       ///< 是否有待处理的读取
    } m_handshake;

    /**
     * @brief 接收状态
     */
    struct RecvState {
        char* plain_buffer = nullptr;                    ///< 明文缓冲区
        size_t plain_length = 0;                         ///< 明文长度
        bool result_set = false;                         ///< 结果是否已设置
        std::expected<Bytes, SslError> result{};         ///< 接收结果
    } m_recv;

    /**
     * @brief 发送状态
     */
    struct SendState {
        const char* plain_buffer = nullptr;              ///< 明文缓冲区
        size_t plain_length = 0;                         ///< 明文总长度
        size_t plain_offset = 0;                         ///< 当前发送偏移
        bool read_pending = false;                       ///< 是否有待处理的读取
        bool result_set = false;                         ///< 结果是否已设置
        std::expected<size_t, SslError> result{};        ///< 发送结果
    } m_send;

    /**
     * @brief 关闭状态
     */
    struct ShutdownState {
        bool result_set = false;                         ///< 结果是否已设置
        std::expected<void, SslError> result{};          ///< 关闭结果
        bool wait_read_after_write = false;              ///< 写入后是否等待读取
        bool read_pending = false;                       ///< 是否有待处理的读取
    } m_shutdown;
    std::vector<char> m_handshake_buffer;                ///< 握手密文缓冲区
    std::vector<char> m_shutdown_buffer;                 ///< 关闭密文缓冲区
    std::vector<char> m_recv_cipher_buffer;              ///< 接收密文缓冲区
    std::vector<char> m_send_cipher_buffer;              ///< 发送密文缓冲区
};

/**
 * @brief SSL 状态机协程可等待对象
 * @tparam MachineT 满足 SslAwaitableStateMachine 概念的状态机类型
 * @details 将 SSL 状态机与协程调度器集成，通过 co_await 驱动多步 SSL 操作。
 * 继承 SequenceAwaitableBase 以支持 IO 序列调度，继承 TimeoutSupport 以支持超时。
 */
template <SslAwaitableStateMachine MachineT>
class SslStateMachineAwaitable
    : public SequenceAwaitableBase
    , public TimeoutSupport<SslStateMachineAwaitable<MachineT>> {
public:
    using result_type = typename MachineT::result_type;  ///< 操作结果类型

    /**
     * @brief 构造状态机可等待对象
     * @param controller IO 控制器
     * @param socket SSL Socket 指针
     * @param machine 状态机实例
     */
    SslStateMachineAwaitable(IOController* controller, SslSocket* socket, MachineT machine)
        : SequenceAwaitableBase(controller)
        , m_socket(socket)
        , m_machine(std::move(machine))
        , m_driver(socket) {}

    /**
     * @brief 检查操作是否已完成（无需挂起）
     * @return 如果已有结果或错误则返回 true
     */
    bool await_ready()
    {
        return m_result_set || m_error.has_value() || SequenceAwaitableBase::m_error.has_value();
    }

    /**
     * @brief 挂起协程并注册到调度器
     * @tparam Promise 协程 promise 类型
     * @param handle 协程句柄
     * @return 是否需要挂起
     */
    template <typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle)
    {
        if (!m_context_bound) {
            galay::kernel::detail::bindAwaitContextIfSupported(
                m_machine,
                galay::kernel::detail::makeAwaitContext(handle));
            m_context_bound = true;
        }
        return SequenceAwaitableBase::await_suspend(handle);
    }

    /**
     * @brief 恢复协程时返回操作结果
     * @return 操作结果，失败时返回包含错误的 expected
     */
    auto await_resume() -> result_type
    {
        onCompleted();
        if (m_result_set) {
            return std::move(*m_result);
        }
        if (m_error.has_value()) {
            if constexpr (detail::is_expected_v<result_type>) {
                using ErrorT = typename detail::expected_traits<result_type>::error_type;
                if constexpr (std::is_constructible_v<ErrorT, SslError>) {
                    return std::unexpected(ErrorT(*m_error));
                }
            }
        }
        if (SequenceAwaitableBase::m_error.has_value()) {
            // Sequence registration can fail immediately before the SSL machine
            // produces a driver-level SslError. Bridge that base error instead of aborting.
            if constexpr (detail::is_expected_v<result_type>) {
                using ErrorT = typename detail::expected_traits<result_type>::error_type;
                if constexpr (std::is_constructible_v<ErrorT, SslError>) {
                    return std::unexpected(ErrorT(bridgeSequenceError(*SequenceAwaitableBase::m_error)));
                }
            }
        }
        std::abort();
    }

    /**
     * @brief 获取当前活跃的 IO 任务
     * @return IO 任务指针，无活跃任务时返回 nullptr
     */
    IOTask* front() override
    {
        return m_has_active_task ? &m_active_task : nullptr;
    }

    /**
     * @brief 获取当前活跃的 IO 任务（const 版本）
     */
    const IOTask* front() const override
    {
        return m_has_active_task ? &m_active_task : nullptr;
    }

    /**
     * @brief 移除队首任务
     */
    void popFront() override
    {
        clearActiveTask();
    }

    /**
     * @brief 检查任务队列是否为空
     * @return 无活跃任务时返回 true
     */
    bool empty() const override
    {
        return !m_has_active_task;
    }

    /**
     * @brief 标记操作超时
     * @details 清除活跃任务，向状态机注入超时错误，继续驱动状态机
     */
    void markTimeout()
    {
        clearActiveTask();
        const auto timeout = std::unexpected(SslError(SslErrorCode::kTimeout));
        switch (m_running_signal) {
        case SslMachineSignal::kHandshake:
            m_machine.onHandshake(std::expected<void, SslError>(timeout));
            break;
        case SslMachineSignal::kRecv:
            m_machine.onRecv(std::expected<Bytes, SslError>(timeout));
            break;
        case SslMachineSignal::kSend:
            m_machine.onSend(std::expected<size_t, SslError>(timeout));
            break;
        case SslMachineSignal::kShutdown:
            m_machine.onShutdown(std::expected<void, SslError>(timeout));
            break;
        case SslMachineSignal::kContinue:
        case SslMachineSignal::kComplete:
        case SslMachineSignal::kFail:
            break;
        }
        m_running_signal = SslMachineSignal::kContinue;
        (void)pump();
        if (!m_result_set && !m_error.has_value()) {
            setFailure(SslError(SslErrorCode::kTimeout));
        }
    }

#ifdef USE_IOURING
    SequenceProgress prepareForSubmit() override
    {
        return pump();
    }

    SequenceProgress onActiveEvent(struct io_uring_cqe* cqe, GHandle handle) override
    {
        if (!m_has_active_task) {
            return pump();
        }
        if (m_active_kind == ActiveKind::kRead) {
            if (!m_driver.recvContext().handleComplete(cqe, handle)) {
                return SequenceProgress::kNeedWait;
            }
            auto io_result = std::move(m_driver.recvContext().m_result);
            clearActiveTask();
            m_driver.onRead(std::move(io_result));
            return pump();
        }
        if (m_active_kind == ActiveKind::kWrite) {
            if (!m_driver.sendContext().handleComplete(cqe, handle)) {
                return SequenceProgress::kNeedWait;
            }
            auto io_result = std::move(m_driver.sendContext().m_result);
            clearActiveTask();
            m_driver.onWrite(std::move(io_result));
            return pump();
        }
        setFailure(SslError(SslErrorCode::kUnknown));
        return SequenceProgress::kCompleted;
    }
#else
    SequenceProgress prepareForSubmit(GHandle handle) override
    {
        for (size_t i = 0; i < kInlineTransitionCap; ++i) {
            const SequenceProgress progress = pump();
            if (progress == SequenceProgress::kCompleted) {
                return progress;
            }
            if (!m_has_active_task) {
                return SequenceProgress::kCompleted;
            }
            if (m_active_kind == ActiveKind::kRead) {
                if (!m_driver.recvContext().handleComplete(handle)) {
                    return SequenceProgress::kNeedWait;
                }
                auto io_result = std::move(m_driver.recvContext().m_result);
                clearActiveTask();
                m_driver.onRead(std::move(io_result));
                continue;
            }
            if (m_active_kind == ActiveKind::kWrite) {
                if (!m_driver.sendContext().handleComplete(handle)) {
                    return SequenceProgress::kNeedWait;
                }
                auto io_result = std::move(m_driver.sendContext().m_result);
                clearActiveTask();
                m_driver.onWrite(std::move(io_result));
                continue;
            }
            setFailure(SslError(SslErrorCode::kUnknown));
            return SequenceProgress::kCompleted;
        }
        setFailure(SslError(SslErrorCode::kUnknown));
        clearActiveTask();
        return SequenceProgress::kCompleted;
    }

    SequenceProgress onActiveEvent(GHandle handle) override
    {
        if (!m_has_active_task) {
            return prepareForSubmit(handle);
        }
        if (m_active_kind == ActiveKind::kRead) {
            if (!m_driver.recvContext().handleComplete(handle)) {
                return SequenceProgress::kNeedWait;
            }
            auto io_result = std::move(m_driver.recvContext().m_result);
            clearActiveTask();
            m_driver.onRead(std::move(io_result));
            return prepareForSubmit(handle);
        }
        if (m_active_kind == ActiveKind::kWrite) {
            if (!m_driver.sendContext().handleComplete(handle)) {
                return SequenceProgress::kNeedWait;
            }
            auto io_result = std::move(m_driver.sendContext().m_result);
            clearActiveTask();
            m_driver.onWrite(std::move(io_result));
            return prepareForSubmit(handle);
        }
        setFailure(SslError(SslErrorCode::kUnknown));
        return SequenceProgress::kCompleted;
    }
#endif

private:
    enum class ActiveKind : uint8_t {
        kNone,
        kRead,
        kWrite,
    };

    static constexpr size_t kInlineTransitionCap = 64;

    static SslError bridgeSequenceError(const IOError& error)
    {
        if (IOError::contains(error.code(), kTimeout)) {
            return SslError(SslErrorCode::kTimeout);
        }
        if (IOError::contains(error.code(), kDisconnectError)) {
            return SslError(SslErrorCode::kPeerClosed);
        }
        if (IOError::contains(error.code(), kReadFailed)) {
            return SslError(SslErrorCode::kReadFailed);
        }
        if (IOError::contains(error.code(), kWriteFailed)) {
            return SslError(SslErrorCode::kWriteFailed);
        }
        return SslError(SslErrorCode::kUnknown);
    }

    void setFailure(SslError error)
    {
        if constexpr (detail::is_expected_v<result_type>) {
            using ErrorT = typename detail::expected_traits<result_type>::error_type;
            if constexpr (std::is_constructible_v<ErrorT, SslError>) {
                m_result = std::unexpected(ErrorT(std::move(error)));
                m_result_set = true;
                return;
            }
        }
        m_error = std::move(error);
    }

    void activateRead()
    {
        m_active_task = IOTask{RECV, nullptr, &m_driver.recvContext()};
        m_has_active_task = true;
        m_active_kind = ActiveKind::kRead;
    }

    void activateWrite()
    {
        m_active_task = IOTask{SEND, nullptr, &m_driver.sendContext()};
        m_has_active_task = true;
        m_active_kind = ActiveKind::kWrite;
    }

    void clearActiveTask()
    {
        m_active_task = IOTask{};
        m_has_active_task = false;
        m_active_kind = ActiveKind::kNone;
    }

    void deliverDriverResult()
    {
        switch (m_running_signal) {
        case SslMachineSignal::kHandshake:
            m_machine.onHandshake(m_driver.takeHandshakeResult());
            break;
        case SslMachineSignal::kRecv:
            m_machine.onRecv(m_driver.takeRecvResult());
            break;
        case SslMachineSignal::kSend:
            m_machine.onSend(m_driver.takeSendResult());
            break;
        case SslMachineSignal::kShutdown:
            m_machine.onShutdown(m_driver.takeShutdownResult());
            break;
        default:
            setFailure(SslError(SslErrorCode::kUnknown));
            break;
        }
        m_running_signal = SslMachineSignal::kContinue;
    }

    SequenceProgress startAction(SslMachineAction<result_type> action)
    {
        switch (action.signal) {
        case SslMachineSignal::kContinue:
            return SequenceProgress::kNeedWait;
        case SslMachineSignal::kHandshake:
            m_running_signal = SslMachineSignal::kHandshake;
            m_driver.startHandshake();
            return SequenceProgress::kNeedWait;
        case SslMachineSignal::kRecv:
            if (action.read_buffer == nullptr && action.read_length != 0) {
                setFailure(SslError(SslErrorCode::kReadFailed));
                return SequenceProgress::kCompleted;
            }
            m_running_signal = SslMachineSignal::kRecv;
            m_driver.startRecv(action.read_buffer, action.read_length);
            return SequenceProgress::kNeedWait;
        case SslMachineSignal::kSend:
            if (action.write_buffer == nullptr && action.write_length != 0) {
                setFailure(SslError(SslErrorCode::kWriteFailed));
                return SequenceProgress::kCompleted;
            }
            m_running_signal = SslMachineSignal::kSend;
            m_driver.startSend(action.write_buffer, action.write_length);
            return SequenceProgress::kNeedWait;
        case SslMachineSignal::kShutdown:
            m_running_signal = SslMachineSignal::kShutdown;
            m_driver.startShutdown();
            return SequenceProgress::kNeedWait;
        case SslMachineSignal::kComplete:
            if (!action.result.has_value()) {
                setFailure(SslError(SslErrorCode::kUnknown));
                return SequenceProgress::kCompleted;
            }
            m_result = std::move(*action.result);
            m_result_set = true;
            return SequenceProgress::kCompleted;
        case SslMachineSignal::kFail:
            setFailure(action.error.value_or(SslError(SslErrorCode::kUnknown)));
            return SequenceProgress::kCompleted;
        }
        setFailure(SslError(SslErrorCode::kUnknown));
        return SequenceProgress::kCompleted;
    }

    SequenceProgress pump()
    {
        for (size_t i = 0; i < kInlineTransitionCap; ++i) {
            if (m_result_set || m_error.has_value()) {
                return SequenceProgress::kCompleted;
            }
            if (m_has_active_task) {
                return SequenceProgress::kNeedWait;
            }

            if (m_running_signal != SslMachineSignal::kContinue) {
                const auto wait = m_driver.poll();
                if (m_driver.completed()) {
                    deliverDriverResult();
                    continue;
                }
                if (wait.kind == SslOperationDriver::WaitKind::kRead) {
                    activateRead();
                    return SequenceProgress::kNeedWait;
                }
                if (wait.kind == SslOperationDriver::WaitKind::kWrite) {
                    activateWrite();
                    return SequenceProgress::kNeedWait;
                }
                setFailure(SslError(SslErrorCode::kUnknown));
                return SequenceProgress::kCompleted;
            }

            auto action = m_machine.advance();
            const SequenceProgress progress = startAction(std::move(action));
            if (progress == SequenceProgress::kCompleted) {
                return progress;
            }
            continue;
        }

        setFailure(SslError(SslErrorCode::kUnknown));
        clearActiveTask();
        return SequenceProgress::kCompleted;
    }

    SslSocket* m_socket = nullptr;
    MachineT m_machine;
    SslOperationDriver m_driver;
    IOTask m_active_task{};
    bool m_has_active_task = false;
    ActiveKind m_active_kind = ActiveKind::kNone;
    SslMachineSignal m_running_signal = SslMachineSignal::kContinue;
    bool m_context_bound = false;
    std::optional<result_type> m_result;
    bool m_result_set = false;
    std::optional<SslError> m_error;
};

/**
 * @brief SSL 状态机构建器
 * @tparam MachineT 状态机类型
 * @details 从状态机实例构建 SslStateMachineAwaitable 对象
 */
template <SslAwaitableStateMachine MachineT>
class SslStateMachineBuilder
{
public:
    /**
     * @brief 构造状态机构建器
     * @param controller IO 控制器
     * @param socket SSL Socket 指针
     * @param machine 状态机实例
     */
    SslStateMachineBuilder(IOController* controller, SslSocket* socket, MachineT machine)
        : m_controller(controller)
        , m_socket(socket)
        , m_machine(std::move(machine)) {}

    /**
     * @brief 构建可等待对象（左值引用）
     * @return 状态机可等待对象
     */
    auto build() & -> SslStateMachineAwaitable<MachineT>
    {
        return SslStateMachineAwaitable<MachineT>(m_controller, m_socket, std::move(m_machine));
    }

    /**
     * @brief 构建可等待对象（右值引用）
     * @return 状态机可等待对象
     */
    auto build() && -> SslStateMachineAwaitable<MachineT>
    {
        return SslStateMachineAwaitable<MachineT>(m_controller, m_socket, std::move(m_machine));
    }

private:
    IOController* m_controller;  ///< IO 控制器
    SslSocket* m_socket;         ///< SSL Socket 指针
    MachineT m_machine;          ///< 状态机实例
};

/**
 * @brief 单步握手状态机
 * @details 执行一次 SSL 握手操作，完成后返回结果
 */
namespace detail {

struct SslSingleHandshakeMachine {
    using result_type = std::expected<void, SslError>;  ///< 结果类型

    /**
     * @brief 推进状态机
     * @return 下一步动作
     */
    SslMachineAction<result_type> advance()
    {
        if (m_result.has_value()) {
            return SslMachineAction<result_type>::complete(std::move(*m_result));
        }
        return SslMachineAction<result_type>::handshake();
    }

    void onHandshake(std::expected<void, SslError> result) { m_result = std::move(result); }  ///< 处理握手结果
    void onRecv(std::expected<Bytes, SslError>) {}     ///< 未使用
    void onSend(std::expected<size_t, SslError>) {}    ///< 未使用
    void onShutdown(std::expected<void, SslError>) {}  ///< 未使用

    std::optional<result_type> m_result;  ///< 握手结果
};

/**
 * @brief 单步接收状态机
 * @details 执行一次 SSL 接收操作，完成后返回接收到的数据
 */
struct SslSingleRecvMachine {
    using result_type = std::expected<Bytes, SslError>;  ///< 结果类型

    /**
     * @brief 构造接收状态机
     * @param buffer 接收缓冲区
     * @param length 缓冲区大小
     */
    SslSingleRecvMachine(char* buffer, size_t length)
        : m_buffer(buffer)
        , m_length(length) {}

    /**
     * @brief 推进状态机
     * @return 下一步动作
     */
    SslMachineAction<result_type> advance()
    {
        if (m_result.has_value()) {
            return SslMachineAction<result_type>::complete(std::move(*m_result));
        }
        return SslMachineAction<result_type>::recv(m_buffer, m_length);
    }

    void onHandshake(std::expected<void, SslError>) {}                      ///< 未使用
    void onRecv(std::expected<Bytes, SslError> result) { m_result = std::move(result); }  ///< 处理接收结果
    void onSend(std::expected<size_t, SslError>) {}     ///< 未使用
    void onShutdown(std::expected<void, SslError>) {}   ///< 未使用

    char* m_buffer = nullptr;              ///< 接收缓冲区
    size_t m_length = 0;                   ///< 缓冲区大小
    std::optional<result_type> m_result;   ///< 接收结果
};

/**
 * @brief 单步发送状态机
 * @details 执行一次 SSL 发送操作，完成后返回已发送字节数
 */
struct SslSingleSendMachine {
    using result_type = std::expected<size_t, SslError>;  ///< 结果类型

    /**
     * @brief 构造发送状态机
     * @param buffer 发送数据
     * @param length 数据长度
     */
    SslSingleSendMachine(const char* buffer, size_t length)
        : m_buffer(buffer)
        , m_length(length) {}

    /**
     * @brief 推进状态机
     * @return 下一步动作
     */
    SslMachineAction<result_type> advance()
    {
        if (m_result.has_value()) {
            return SslMachineAction<result_type>::complete(std::move(*m_result));
        }
        return SslMachineAction<result_type>::send(m_buffer, m_length);
    }

    void onHandshake(std::expected<void, SslError>) {}                          ///< 未使用
    void onRecv(std::expected<Bytes, SslError>) {}       ///< 未使用
    void onSend(std::expected<size_t, SslError> result) { m_result = std::move(result); }  ///< 处理发送结果
    void onShutdown(std::expected<void, SslError>) {}    ///< 未使用

    const char* m_buffer = nullptr;        ///< 发送数据指针
    size_t m_length = 0;                   ///< 数据长度
    std::optional<result_type> m_result;   ///< 发送结果
};

/**
 * @brief 单步关闭状态机
 * @details 执行一次 SSL 关闭操作，完成后返回结果
 */
struct SslSingleShutdownMachine {
    using result_type = std::expected<void, SslError>;  ///< 结果类型

    /**
     * @brief 推进状态机
     * @return 下一步动作
     */
    SslMachineAction<result_type> advance()
    {
        if (m_result.has_value()) {
            return SslMachineAction<result_type>::complete(std::move(*m_result));
        }
        return SslMachineAction<result_type>::shutdown();
    }

    void onHandshake(std::expected<void, SslError>) {}                             ///< 未使用
    void onRecv(std::expected<Bytes, SslError>) {}        ///< 未使用
    void onSend(std::expected<size_t, SslError>) {}       ///< 未使用
    void onShutdown(std::expected<void, SslError> result) { m_result = std::move(result); }  ///< 处理关闭结果

    std::optional<result_type> m_result;   ///< 关闭结果
};

/**
 * @brief 线性多步骤 SSL 状态机
 * @tparam ResultT 结果类型
 * @tparam InlineN 内联步骤容量
 * @tparam FlowT 用户流水线类型
 * @details 按顺序执行一组预定义的 SSL 操作步骤（握手/接收/发送/关闭/解析/本地），
 * 每步可附带用户回调处理结果。支持解析步骤的循环重入（如协议解析需要更多数据时）。
 */
template <typename ResultT, size_t InlineN, typename FlowT>
class SslLinearMachine
{
public:
    using result_type = ResultT;                              ///< 结果类型
    using OpsT = SslBuilderOps<ResultT, InlineN>;             ///< 操作辅助类型

    static constexpr size_t kInvalidIndex = static_cast<size_t>(-1);  ///< 无效索引常量

    /**
     * @brief 步骤节点类型
     */
    enum class NodeKind : uint8_t {
        kHandshake,  ///< 握手步骤
        kRecv,       ///< 接收步骤
        kSend,       ///< 发送步骤
        kShutdown,   ///< 关闭步骤
        kParse,      ///< 解析步骤（可循环重入）
        kLocal,      ///< 本地处理步骤
        kFinish,     ///< 完成步骤
    };

    using HandshakeHandlerFn = void(*)(FlowT*, OpsT&, SslHandshakeContext&);  ///< 握手回调类型
    using RecvHandlerFn = void(*)(FlowT*, OpsT&, SslRecvContext&);            ///< 接收回调类型
    using SendHandlerFn = void(*)(FlowT*, OpsT&, SslSendContext&);            ///< 发送回调类型
    using ShutdownHandlerFn = void(*)(FlowT*, OpsT&, SslShutdownContext&);    ///< 关闭回调类型
    using LocalHandlerFn = void(*)(FlowT*, OpsT&);                            ///< 本地回调类型
    using ParseHandlerFn = ParseStatus(*)(FlowT*, OpsT&);                     ///< 解析回调类型

    /**
     * @brief 步骤节点
     */
    struct Node {
        NodeKind kind = NodeKind::kLocal;              ///< 节点类型
        HandshakeHandlerFn handshake_handler = nullptr; ///< 握手回调
        RecvHandlerFn recv_handler = nullptr;           ///< 接收回调
        SendHandlerFn send_handler = nullptr;           ///< 发送回调
        ShutdownHandlerFn shutdown_handler = nullptr;   ///< 关闭回调
        LocalHandlerFn local_handler = nullptr;         ///< 本地回调
        ParseHandlerFn parse_handler = nullptr;         ///< 解析回调
        char* read_buffer = nullptr;                    ///< 读取缓冲区
        const char* write_buffer = nullptr;             ///< 写入缓冲区
        size_t io_length = 0;                           ///< IO 长度
        size_t parse_rearm_recv_index = kInvalidIndex;  ///< 解析步骤重入的接收节点索引
    };

    using NodeList = std::vector<Node>;  ///< 节点列表类型

    /**
     * @brief 构造线性状态机
     * @param controller IO 控制器
     * @param flow 用户流水线指针
     * @param nodes 步骤节点列表
     */
    SslLinearMachine(IOController* controller, FlowT* flow, NodeList nodes)
        : m_flow(flow)
        , m_nodes(std::move(nodes))
    {
        (void)controller;
    }

    /**
     * @brief 创建握手节点
     * @tparam Handler 成员函数指针回调
     * @return 握手步骤节点
     */
    template <auto Handler>
    static Node makeHandshakeNode()
    {
        Node node;
        node.kind = NodeKind::kHandshake;
        node.handshake_handler = &invokeHandshake<Handler>;
        return node;
    }

    /**
     * @brief 创建接收节点
     * @tparam Handler 成员函数指针回调
     * @param buffer 接收缓冲区
     * @param length 缓冲区大小
     * @return 接收步骤节点
     */
    template <auto Handler>
    static Node makeRecvNode(char* buffer, size_t length)
    {
        Node node;
        node.kind = NodeKind::kRecv;
        node.recv_handler = &invokeRecv<Handler>;
        node.read_buffer = buffer;
        node.io_length = length;
        return node;
    }

    /**
     * @brief 创建发送节点
     * @tparam Handler 成员函数指针回调
     * @param buffer 发送数据
     * @param length 数据长度
     * @return 发送步骤节点
     */
    template <auto Handler>
    static Node makeSendNode(const char* buffer, size_t length)
    {
        Node node;
        node.kind = NodeKind::kSend;
        node.send_handler = &invokeSend<Handler>;
        node.write_buffer = buffer;
        node.io_length = length;
        return node;
    }

    /**
     * @brief 创建关闭节点
     * @tparam Handler 成员函数指针回调
     * @return 关闭步骤节点
     */
    template <auto Handler>
    static Node makeShutdownNode()
    {
        Node node;
        node.kind = NodeKind::kShutdown;
        node.shutdown_handler = &invokeShutdown<Handler>;
        return node;
    }

    /**
     * @brief 创建本地处理节点
     * @tparam Handler 成员函数指针回调
     * @return 本地步骤节点
     */
    template <auto Handler>
    static Node makeLocalNode()
    {
        Node node;
        node.kind = NodeKind::kLocal;
        node.local_handler = &invokeLocal<Handler>;
        return node;
    }

    /**
     * @brief 创建完成节点
     * @tparam Handler 成员函数指针回调
     * @return 完成步骤节点
     */
    template <auto Handler>
    static Node makeFinishNode()
    {
        Node node;
        node.kind = NodeKind::kFinish;
        node.local_handler = &invokeLocal<Handler>;
        return node;
    }

    /**
     * @brief 创建解析节点
     * @tparam Handler 成员函数指针回调
     * @param rearm_recv_index 解析需要更多数据时回退到的接收节点索引
     * @return 解析步骤节点
     */
    template <auto Handler>
    static Node makeParseNode(size_t rearm_recv_index)
    {
        Node node;
        node.kind = NodeKind::kParse;
        node.parse_handler = &invokeParse<Handler>;
        node.parse_rearm_recv_index = rearm_recv_index;
        return node;
    }

    /**
     * @brief 绑定协程上下文到流水线
     * @param ctx 协程上下文
     */
    void onAwaitContext(const AwaitContext& ctx)
    {
        if constexpr (requires(FlowT& flow, const AwaitContext& context) {
            flow.onAwaitContext(context);
        }) {
            if (m_flow != nullptr) {
                m_flow->onAwaitContext(ctx);
            }
        }
    }

    /**
     * @brief 推进线性状态机
     * @details 根据当前游标位置执行对应步骤，返回下一步动作
     * @return 当前步骤的动作
     */
    SslMachineAction<result_type> advance()
    {
        if (m_result.has_value()) {
            return SslMachineAction<result_type>::complete(std::move(*m_result));
        }
        if (m_error.has_value()) {
            return SslMachineAction<result_type>::fail(*m_error);
        }
        if (m_cursor >= m_nodes.size()) {
            setError(SslError(SslErrorCode::kUnknown));
            return emitActionFromOutcome();
        }

        const Node& node = m_nodes[m_cursor];
        switch (node.kind) {
        case NodeKind::kHandshake:
            m_pending_kind = NodeKind::kHandshake;
            m_pending_index = m_cursor;
            return SslMachineAction<result_type>::handshake();
        case NodeKind::kRecv:
            m_recv_context.m_buffer = node.read_buffer;
            m_recv_context.m_length = node.io_length;
            m_pending_kind = NodeKind::kRecv;
            m_pending_index = m_cursor;
            return SslMachineAction<result_type>::recv(node.read_buffer, node.io_length);
        case NodeKind::kSend:
            m_send_context.m_buffer = node.write_buffer;
            m_send_context.m_length = node.io_length;
            m_pending_kind = NodeKind::kSend;
            m_pending_index = m_cursor;
            return SslMachineAction<result_type>::send(node.write_buffer, node.io_length);
        case NodeKind::kShutdown:
            m_pending_kind = NodeKind::kShutdown;
            m_pending_index = m_cursor;
            return SslMachineAction<result_type>::shutdown();
        case NodeKind::kParse:
            return runParse(node);
        case NodeKind::kLocal:
        case NodeKind::kFinish:
            return runLocal(node);
        }
        setError(SslError(SslErrorCode::kUnknown));
        return emitActionFromOutcome();
    }

    /**
     * @brief 处理握手结果
     * @param result 握手结果
     */
    void onHandshake(std::expected<void, SslError> result)
    {
        if (m_pending_kind != NodeKind::kHandshake || m_pending_index >= m_nodes.size()) {
            setError(SslError(SslErrorCode::kUnknown));
            return;
        }

        const bool has_value = result.has_value();
        std::optional<SslError> error;
        if (!has_value) {
            error = result.error();
        }
        m_handshake_context.m_result = std::move(result);

        const Node& node = m_nodes[m_pending_index];
        invokeHandshakeNode(node);
        clearPending();

        if (absorbOpsOutcome()) {
            return;
        }
        if (error.has_value()) {
            setError(std::move(*error));
            return;
        }
        ++m_cursor;
    }

    /**
     * @brief 处理接收结果
     * @param result 接收结果
     */
    void onRecv(std::expected<Bytes, SslError> result)
    {
        if (m_pending_kind != NodeKind::kRecv || m_pending_index >= m_nodes.size()) {
            setError(SslError(SslErrorCode::kUnknown));
            return;
        }

        const bool has_value = result.has_value();
        std::optional<SslError> error;
        if (!has_value) {
            error = result.error();
        }
        m_recv_context.m_result = std::move(result);

        const Node& node = m_nodes[m_pending_index];
        invokeRecvNode(node);
        clearPending();

        if (absorbOpsOutcome()) {
            return;
        }
        if (error.has_value()) {
            setError(std::move(*error));
            return;
        }
        ++m_cursor;
    }

    /**
     * @brief 处理发送结果
     * @param result 发送结果
     */
    void onSend(std::expected<size_t, SslError> result)
    {
        if (m_pending_kind != NodeKind::kSend || m_pending_index >= m_nodes.size()) {
            setError(SslError(SslErrorCode::kUnknown));
            return;
        }

        const bool has_value = result.has_value();
        std::optional<SslError> error;
        if (!has_value) {
            error = result.error();
        }
        m_send_context.m_result = std::move(result);

        const Node& node = m_nodes[m_pending_index];
        invokeSendNode(node);
        clearPending();

        if (absorbOpsOutcome()) {
            return;
        }
        if (error.has_value()) {
            setError(std::move(*error));
            return;
        }
        ++m_cursor;
    }

    /**
     * @brief 处理关闭结果
     * @param result 关闭结果
     */
    void onShutdown(std::expected<void, SslError> result)
    {
        if (m_pending_kind != NodeKind::kShutdown || m_pending_index >= m_nodes.size()) {
            setError(SslError(SslErrorCode::kUnknown));
            return;
        }

        const bool has_value = result.has_value();
        std::optional<SslError> error;
        if (!has_value) {
            error = result.error();
        }
        m_shutdown_context.m_result = std::move(result);

        const Node& node = m_nodes[m_pending_index];
        invokeShutdownNode(node);
        clearPending();

        if (absorbOpsOutcome()) {
            return;
        }
        if (error.has_value()) {
            setError(std::move(*error));
            return;
        }
        ++m_cursor;
    }

private:
    template <auto Handler>
    static void invokeHandshake(FlowT* flow, OpsT& ops, SslHandshakeContext& ctx)
    {
        (flow->*Handler)(ops, ctx);
    }

    template <auto Handler>
    static void invokeRecv(FlowT* flow, OpsT& ops, SslRecvContext& ctx)
    {
        (flow->*Handler)(ops, ctx);
    }

    template <auto Handler>
    static void invokeSend(FlowT* flow, OpsT& ops, SslSendContext& ctx)
    {
        (flow->*Handler)(ops, ctx);
    }

    template <auto Handler>
    static void invokeShutdown(FlowT* flow, OpsT& ops, SslShutdownContext& ctx)
    {
        (flow->*Handler)(ops, ctx);
    }

    template <auto Handler>
    static void invokeLocal(FlowT* flow, OpsT& ops)
    {
        (flow->*Handler)(ops);
    }

    template <auto Handler>
    static ParseStatus invokeParse(FlowT* flow, OpsT& ops)
    {
        return (flow->*Handler)(ops);
    }

    void invokeHandshakeNode(const Node& node)
    {
        if (node.handshake_handler == nullptr) {
            setError(SslError(SslErrorCode::kUnknown));
            return;
        }
        m_ops_owner.reset();
        OpsT ops(m_ops_owner);
        node.handshake_handler(m_flow, ops, m_handshake_context);
    }

    void invokeRecvNode(const Node& node)
    {
        if (node.recv_handler == nullptr) {
            setError(SslError(SslErrorCode::kUnknown));
            return;
        }
        m_ops_owner.reset();
        OpsT ops(m_ops_owner);
        node.recv_handler(m_flow, ops, m_recv_context);
    }

    void invokeSendNode(const Node& node)
    {
        if (node.send_handler == nullptr) {
            setError(SslError(SslErrorCode::kUnknown));
            return;
        }
        m_ops_owner.reset();
        OpsT ops(m_ops_owner);
        node.send_handler(m_flow, ops, m_send_context);
    }

    void invokeShutdownNode(const Node& node)
    {
        if (node.shutdown_handler == nullptr) {
            setError(SslError(SslErrorCode::kUnknown));
            return;
        }
        m_ops_owner.reset();
        OpsT ops(m_ops_owner);
        node.shutdown_handler(m_flow, ops, m_shutdown_context);
    }

    SslMachineAction<result_type> runLocal(const Node& node)
    {
        if (node.local_handler == nullptr) {
            setError(SslError(SslErrorCode::kUnknown));
            return emitActionFromOutcome();
        }

        m_ops_owner.reset();
        OpsT ops(m_ops_owner);
        node.local_handler(m_flow, ops);

        if (absorbOpsOutcome()) {
            return emitActionFromOutcome();
        }
        ++m_cursor;
        return SslMachineAction<result_type>::continue_();
    }

    SslMachineAction<result_type> runParse(const Node& node)
    {
        if (node.parse_handler == nullptr) {
            setError(SslError(SslErrorCode::kUnknown));
            return emitActionFromOutcome();
        }

        m_ops_owner.reset();
        OpsT ops(m_ops_owner);
        const ParseStatus status = node.parse_handler(m_flow, ops);

        if (absorbOpsOutcome()) {
            return emitActionFromOutcome();
        }

        switch (status) {
        case ParseStatus::kNeedMore:
            if (node.parse_rearm_recv_index == kInvalidIndex ||
                node.parse_rearm_recv_index >= m_nodes.size() ||
                m_nodes[node.parse_rearm_recv_index].kind != NodeKind::kRecv) {
                setError(SslError(SslErrorCode::kUnknown));
                return emitActionFromOutcome();
            }
            m_cursor = node.parse_rearm_recv_index;
            return SslMachineAction<result_type>::continue_();
        case ParseStatus::kContinue:
            return SslMachineAction<result_type>::continue_();
        case ParseStatus::kCompleted:
            ++m_cursor;
            return SslMachineAction<result_type>::continue_();
        }
        setError(SslError(SslErrorCode::kUnknown));
        return emitActionFromOutcome();
    }

    bool absorbOpsOutcome()
    {
        if (m_ops_owner.hasResultValue()) {
            auto result = m_ops_owner.takeResultValue();
            if (result.has_value()) {
                m_result = std::move(*result);
            } else {
                setError(SslError(SslErrorCode::kUnknown));
            }
            return true;
        }
        if (m_ops_owner.queueUsed()) {
            m_ops_owner.clear();
            setError(SslError(SslErrorCode::kUnknown));
            return true;
        }
        return false;
    }

    void setError(SslError error)
    {
        if constexpr (detail::is_expected_v<result_type>) {
            using ErrorT = typename detail::expected_traits<result_type>::error_type;
            if constexpr (std::is_constructible_v<ErrorT, SslError>) {
                m_result = std::unexpected(ErrorT(std::move(error)));
                return;
            }
        }
        m_error = std::move(error);
    }

    SslMachineAction<result_type> emitActionFromOutcome()
    {
        if (m_result.has_value()) {
            return SslMachineAction<result_type>::complete(std::move(*m_result));
        }
        if (m_error.has_value()) {
            return SslMachineAction<result_type>::fail(*m_error);
        }
        return SslMachineAction<result_type>::continue_();
    }

    void clearPending()
    {
        m_pending_kind = NodeKind::kLocal;
        m_pending_index = kInvalidIndex;
    }

    FlowT* m_flow = nullptr;
    NodeList m_nodes;
    size_t m_cursor = 0;
    NodeKind m_pending_kind = NodeKind::kLocal;
    size_t m_pending_index = kInvalidIndex;
    size_t m_last_recv_index = kInvalidIndex;

    SslBuilderOutcome<ResultT> m_ops_owner;
    SslHandshakeContext m_handshake_context;
    SslRecvContext m_recv_context;
    SslSendContext m_send_context;
    SslShutdownContext m_shutdown_context;
    std::optional<result_type> m_result;
    std::optional<SslError> m_error;
};

} // namespace detail

/**
 * @brief SSL 异步操作流水线构建器
 * @tparam ResultT 结果类型
 * @tparam InlineN 内联步骤容量
 * @tparam FlowT 用户流水线类型（void 表示仅使用自定义状态机）
 * @details 提供链式 API 构建多步骤 SSL 操作流水线。
 * 每步通过成员函数指针回调处理结果，支持握手/接收/发送/关闭/解析/本地处理步骤。
 *
 * @code
 * auto awaitable = SslAwaitableBuilder<ExpectedType>(controller, socket, flow)
 *     .handshake<&Flow::onConnect>()
 *     .recv<&Flow::onResponse>(buffer, sizeof(buffer))
 *     .parse<&Flow::onParse>()
 *     .finish<&Flow::onDone>()
 *     .build();
 * auto result = co_await awaitable;
 * @endcode
 */
template <typename ResultT, size_t InlineN = 4, typename FlowT = void>
class SslAwaitableBuilder
{
public:
    using MachineT = detail::SslLinearMachine<ResultT, InlineN, FlowT>;  ///< 线性状态机类型
    using MachineNode = typename MachineT::Node;                          ///< 节点类型

    /**
     * @brief 构建流水线构建器
     * @param controller IO 控制器
     * @param socket SSL Socket 指针
     * @param flow 用户流水线引用
     */
    SslAwaitableBuilder(IOController* controller, SslSocket* socket, FlowT& flow)
        : m_controller(controller)
        , m_socket(socket)
        , m_flow(&flow)
    {
        m_nodes.reserve(InlineN);
    }

    /**
     * @brief 从自定义状态机构建器创建
     * @tparam MachineTParam 状态机类型
     * @param controller IO 控制器
     * @param socket SSL Socket 指针
     * @param machine 状态机实例
     * @return 状态机构建器
     */
    template <SslAwaitableStateMachine MachineTParam>
    static auto fromStateMachine(IOController* controller, SslSocket* socket, MachineTParam machine)
        -> SslStateMachineBuilder<MachineTParam>
    {
        static_assert(std::same_as<typename MachineTParam::result_type, ResultT>,
                      "SslAwaitableBuilder::fromStateMachine requires matching result_type");
        return SslStateMachineBuilder<MachineTParam>(controller, socket, std::move(machine));
    }

    /**
     * @brief 添加握手步骤
     * @tparam Handler 成员函数指针回调
     * @return 构建器引用（支持链式调用）
     */
    template <auto Handler>
    SslAwaitableBuilder& handshake()
    {
        m_nodes.push_back(MachineT::template makeHandshakeNode<Handler>());
        return *this;
    }

    /**
     * @brief 添加接收步骤
     * @tparam Handler 成员函数指针回调
     * @param buffer 接收缓冲区
     * @param length 缓冲区大小
     * @return 构建器引用
     */
    template <auto Handler>
    SslAwaitableBuilder& recv(char* buffer, size_t length)
    {
        m_nodes.push_back(MachineT::template makeRecvNode<Handler>(buffer, length));
        m_last_recv_index = m_nodes.size() - 1;
        return *this;
    }

    /**
     * @brief 添加发送步骤
     * @tparam Handler 成员函数指针回调
     * @param buffer 发送数据
     * @param length 数据长度
     * @return 构建器引用
     */
    template <auto Handler>
    SslAwaitableBuilder& send(const char* buffer, size_t length)
    {
        m_nodes.push_back(MachineT::template makeSendNode<Handler>(buffer, length));
        return *this;
    }

    /**
     * @brief 添加关闭步骤
     * @tparam Handler 成员函数指针回调
     * @return 构建器引用
     */
    template <auto Handler>
    SslAwaitableBuilder& shutdown()
    {
        m_nodes.push_back(MachineT::template makeShutdownNode<Handler>());
        return *this;
    }

    /**
     * @brief 添加本地处理步骤
     * @tparam Handler 成员函数指针回调
     * @return 构建器引用
     */
    template <auto Handler>
    SslAwaitableBuilder& local()
    {
        m_nodes.push_back(MachineT::template makeLocalNode<Handler>());
        return *this;
    }

    /**
     * @brief 添加解析步骤（可循环重入到最近的接收步骤）
     * @tparam Handler 成员函数指针回调，返回 ParseStatus
     * @return 构建器引用
     */
    template <auto Handler>
    SslAwaitableBuilder& parse()
    {
        m_nodes.push_back(MachineT::template makeParseNode<Handler>(m_last_recv_index));
        return *this;
    }

    /**
     * @brief 添加完成步骤
     * @tparam Handler 成员函数指针回调
     * @return 构建器引用
     */
    template <auto Handler>
    SslAwaitableBuilder& finish()
    {
        m_nodes.push_back(MachineT::template makeFinishNode<Handler>());
        return *this;
    }

    /**
     * @brief 构建可等待对象（左值引用）
     * @return 状态机可等待对象
     */
    auto build() & -> SslStateMachineAwaitable<MachineT>
    {
        return buildImpl();
    }

    /**
     * @brief 构建可等待对象（右值引用）
     * @return 状态机可等待对象
     */
    auto build() && -> SslStateMachineAwaitable<MachineT>
    {
        return buildImpl();
    }

private:
    auto buildImpl() -> SslStateMachineAwaitable<MachineT>
    {
        return SslStateMachineAwaitable<MachineT>(
            m_controller,
            m_socket,
            MachineT(m_controller, m_flow, std::move(m_nodes))
        );
    }

    IOController* m_controller;                                     ///< IO 控制器
    SslSocket* m_socket;                                            ///< SSL Socket 指针
    FlowT* m_flow;                                                  ///< 用户流水线指针
    std::vector<MachineNode> m_nodes;                               ///< 步骤节点列表
    size_t m_last_recv_index = MachineT::kInvalidIndex;             ///< 最近一次接收步骤的索引
};

/**
 * @brief SSL 异步操作流水线构建器（无用户流水线特化）
 * @tparam ResultT 结果类型
 * @tparam InlineN 内联步骤容量
 * @details 仅支持 fromStateMachine 方式创建可等待对象，不提供链式构建 API
 */
template <typename ResultT, size_t InlineN>
class SslAwaitableBuilder<ResultT, InlineN, void>
{
public:
    /**
     * @brief 从自定义状态机创建
     * @tparam MachineT 状态机类型
     * @param controller IO 控制器
     * @param socket SSL Socket 指针
     * @param machine 状态机实例
     * @return 状态机构建器
     */
    template <SslAwaitableStateMachine MachineT>
    static auto fromStateMachine(IOController* controller, SslSocket* socket, MachineT machine)
        -> SslStateMachineBuilder<MachineT>
    {
        static_assert(std::same_as<typename MachineT::result_type, ResultT>,
                      "SslAwaitableBuilder::fromStateMachine requires matching result_type");
        return SslStateMachineBuilder<MachineT>(controller, socket, std::move(machine));
    }
};

} // namespace galay::ssl

#endif // GALAY_SSL_AWAIT_H
