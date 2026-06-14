/**
 * @file rpc_await.h
 * @brief RPC builder/state-machine awaitable 辅助骨架
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details 为 RPC 仓库内的读写 facade 提供共享的 state-machine helper，
 *          让上层 awaitable 通过 `AwaitableBuilder::fromStateMachine(...)`
 *          暴露，而不是继续继承手写 IO awaitable。
 */

#ifndef GALAY_RPC_AWAIT_H
#define GALAY_RPC_AWAIT_H

#include "rpc/protoc/rpc_error.h"
#include "kernel/kernel/awaitable.h"
#include <utils/cache/ring_buffer.hpp>

#include <array>
#include <expected>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace galay::rpc
{

using namespace galay::kernel;
using ::galay::utils::RingBuffer;

namespace detail {

/**
 * @brief 消耗writev的iovec数组中已写入的字节数
 * @param iovecs iovec数组引用
 * @param consumed 已消耗的字节数
 */
inline void consumeWritevIovecs(std::vector<iovec>& iovecs, size_t consumed)
{
    if (consumed == 0 || iovecs.empty()) {
        return;
    }

    size_t idx = 0;
    while (idx < iovecs.size() && consumed >= iovecs[idx].iov_len) {
        consumed -= iovecs[idx].iov_len;
        ++idx;
    }

    if (idx >= iovecs.size()) {
        iovecs.clear();
        return;
    }

    if (consumed > 0) {
        auto* base = static_cast<char*>(iovecs[idx].iov_base);
        iovecs[idx].iov_base = base + consumed;
        iovecs[idx].iov_len -= consumed;
    }

    if (idx > 0) {
        iovecs.erase(iovecs.begin(),
                     iovecs.begin() + static_cast<std::ptrdiff_t>(idx));
    }
}

/**
 * @brief 准备环形缓冲区的读取窗口
 * @param ring_buffer 环形缓冲区引用
 * @param read_iovecs 输出的iovec数组
 * @param read_iov_count 输出的iovec数量
 * @return 是否有可写空间
 */
inline bool prepareRingBufferReadWindow(RingBuffer& ring_buffer,
                                        std::array<struct iovec, 2>& read_iovecs,
                                        size_t& read_iov_count)
{
    read_iov_count = ring_buffer.getWriteIovecs(read_iovecs);
    size_t writable = 0;
    for (size_t i = 0; i < read_iov_count; ++i) {
        writable += read_iovecs[i].iov_len;
    }
    return writable > 0;
}

/**
 * @brief 将IO错误映射为RPC错误
 * @param io_error 底层IO错误
 * @param default_code 默认RPC错误码
 * @return 映射后的RPC错误
 */
inline RpcError mapRpcReadError(const IOError& io_error,
                                RpcErrorCode default_code = RpcErrorCode::INTERNAL_ERROR)
{
    if (IOError::contains(io_error.code(), kDisconnectError)) {
        return RpcError(RpcErrorCode::CONNECTION_CLOSED, "Connection closed");
    }
    return RpcError::from(io_error, default_code);
}

/**
 * @brief RPC环形缓冲区读取状态基类
 *
 * @details 管理从RingBuffer中读取数据的通用状态机逻辑，
 *          包括准备读取窗口、处理接收错误和对端关闭事件。
 * @tparam ResultT 结果类型，默认为std::expected<bool, RpcError>
 */
template<typename ResultT = std::expected<bool, RpcError>>
class RpcRingBufferReadStateBase
{
public:
    using ResultType = ResultT;

    /**
     * @brief 构造读取状态基类
     * @param ring_buffer 环形缓冲区引用
     */
    explicit RpcRingBufferReadStateBase(RingBuffer& ring_buffer)
        : m_ring_buffer(&ring_buffer)
    {
    }

    /// @brief 准备读取窗口，返回是否成功
    bool prepareReadWindow()
    {
        if (!prepareRingBufferReadWindow(*m_ring_buffer, m_read_iovecs, m_read_iov_count)) {
            m_error.emplace(RpcErrorCode::INTERNAL_ERROR, "No writable ring buffer space");
            return false;
        }
        return true;
    }

    /// @brief 获取接收iovec数组指针
    const struct iovec* recvIovecsData() const { return m_read_iovecs.data(); }
    /// @brief 获取接收iovec数量
    size_t recvIovecsCount() const { return m_read_iov_count; }

    /// @brief 设置接收错误
    void setRecvError(const IOError& io_error)
    {
        m_error = mapRpcReadError(io_error);
    }

    /// @brief 处理对端关闭事件
    void onPeerClosed()
    {
        m_error.emplace(RpcErrorCode::CONNECTION_CLOSED, "Connection closed");
    }

    /// @brief 处理接收到的字节数
    void onBytesReceived(size_t bytes_read)
    {
        m_ring_buffer->produce(bytes_read);
    }

    /// @brief 取出最终结果
    ResultType takeResult()
    {
        if (m_error.has_value()) {
            return std::unexpected(std::move(*m_error));
        }
        return true;
    }

protected:
    /// @brief 获取环形缓冲区引用
    RingBuffer& ringBuffer() { return *m_ring_buffer; }

    /// @brief 设置读取错误
    void setReadError(RpcError error)
    {
        m_error = std::move(error);
    }

private:
    RingBuffer* m_ring_buffer = nullptr;          ///< 环形缓冲区指针
    std::array<struct iovec, 2> m_read_iovecs{}; ///< 读取iovec数组
    size_t m_read_iov_count = 0;                  ///< 读取iovec数量
    std::optional<RpcError> m_error;              ///< 可选的错误信息
};

/**
 * @brief RPC写入状态基类
 *
 * @details 管理向连接写入数据的通用状态机逻辑，
 *          包括准备写入iovec、处理已写字节和发送错误。
 * @tparam ResultT 结果类型，默认为std::expected<bool, RpcError>
 */
template<typename ResultT = std::expected<bool, RpcError>>
class RpcWriteStateBase
{
public:
    using ResultType = ResultT;

    /// @brief 判断写入是否完成（出错或所有iovec已消耗）
    bool isComplete() const
    {
        return m_error.has_value() || m_iovecs.empty();
    }

    /// @brief 准备写入iovec（默认无额外准备）
    bool prepareWriteIovecs()
    {
        return true;
    }

    /// @brief 获取写入iovec数组指针
    const struct iovec* writeIovecsData() const { return m_iovecs.data(); }
    /// @brief 获取写入iovec数量
    size_t writeIovecsCount() const { return m_iovecs.size(); }

    /// @brief 处理已写入的字节数
    void onBytesWritten(size_t bytes_written)
    {
        consumeWritevIovecs(m_iovecs, bytes_written);
    }

    /// @brief 设置发送错误
    void setSendError(const IOError& io_error)
    {
        m_error = RpcError::from(io_error, RpcErrorCode::INTERNAL_ERROR);
    }

    /// @brief 处理零字节写入（发送失败）
    void onZeroWrite()
    {
        m_error = RpcError::from(IOError(kSendFailed, 0), RpcErrorCode::INTERNAL_ERROR);
    }

    /// @brief 取出最终结果
    ResultType takeResult()
    {
        if (m_error.has_value()) {
            return std::unexpected(std::move(*m_error));
        }
        return true;
    }

protected:
    /// @brief 获取可修改的iovec数组引用
    std::vector<struct iovec>& mutableIovecs() { return m_iovecs; }

    /// @brief 设置写入错误
    void setWriteError(RpcError error)
    {
        m_error = std::move(error);
    }

private:
    std::vector<struct iovec> m_iovecs;  ///< 写入iovec数组
    std::optional<RpcError> m_error;      ///< 可选的错误信息
};

/**
 * @brief 向量写入状态（持有数据所有权）
 *
 * @details 将数据向量移动到状态内部，并构建对应的iovec用于writev发送。
 */
class RpcVectorWriteState : public RpcWriteStateBase<>
{
public:
    /**
     * @brief 构造向量写入状态
     * @param data 待发送的数据（移动语义）
     */
    explicit RpcVectorWriteState(std::vector<char>&& data)
        : m_data(std::move(data))
    {
        if (!m_data.empty()) {
            mutableIovecs().push_back(iovec{m_data.data(), m_data.size()});
        }
    }

private:
    std::vector<char> m_data;  ///< 持有的数据缓冲区
};

/**
 * @brief RPC环形缓冲区读取状态机
 *
 * @details 驱动从RingBuffer中逐步读取并解析数据的状态机。
 *          每次advance尝试从环形缓冲区解析，不完整时等待readv填入更多数据。
 * @tparam StateT 状态类型，必须提供parseFromRingBuffer等接口
 */
template<typename StateT>
struct RpcRingBufferReadMachine
{
    using result_type = typename StateT::ResultType;
    static constexpr auto kSequenceOwnerDomain = galay::kernel::SequenceOwnerDomain::Read;

    /**
     * @brief 构造读取状态机
     * @param state 共享的状态对象
     */
    explicit RpcRingBufferReadMachine(std::shared_ptr<StateT> state)
        : m_state(std::move(state))
    {
    }

    /// @brief 推进状态机，返回下一步动作
    MachineAction<result_type> advance()
    {
        if (m_result.has_value()) {
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        if (m_state->parseFromRingBuffer()) {
            m_result = m_state->takeResult();
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        if (!m_state->prepareReadWindow()) {
            m_result = m_state->takeResult();
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        return MachineAction<result_type>::waitReadv(
            m_state->recvIovecsData(),
            m_state->recvIovecsCount());
    }

    /// @brief 处理readv完成事件
    void onRead(std::expected<size_t, IOError> result)
    {
        if (!result.has_value()) {
            m_state->setRecvError(result.error());
            m_result = m_state->takeResult();
            return;
        }

        if (result.value() == 0) {
            m_state->onPeerClosed();
            m_result = m_state->takeResult();
            return;
        }

        m_state->onBytesReceived(result.value());
    }

    /// @brief 写入完成回调（读取状态机中无操作）
    void onWrite(std::expected<size_t, IOError>)
    {
    }

    std::shared_ptr<StateT> m_state;         ///< 状态对象
    std::optional<result_type> m_result;     ///< 缓存的最终结果
};

/**
 * @brief RPC writev写入状态机
 *
 * @details 驱动向连接逐步写入iovec数据的状态机。
 *          每次advance检查是否完成，未完成时等待writev发送更多数据。
 * @tparam StateT 状态类型，必须提供isComplete、prepareWriteIovecs等接口
 */
template<typename StateT>
struct RpcWritevMachine
{
    using result_type = typename StateT::ResultType;
    static constexpr auto kSequenceOwnerDomain = galay::kernel::SequenceOwnerDomain::Write;

    /**
     * @brief 构造写入状态机
     * @param state 共享的状态对象
     */
    explicit RpcWritevMachine(std::shared_ptr<StateT> state)
        : m_state(std::move(state))
    {
    }

    /// @brief 推进状态机，返回下一步动作
    MachineAction<result_type> advance()
    {
        if (m_result.has_value()) {
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        if (m_state->isComplete()) {
            m_result = m_state->takeResult();
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        if (!m_state->prepareWriteIovecs()) {
            m_result = m_state->takeResult();
            return MachineAction<result_type>::complete(std::move(*m_result));
        }

        return MachineAction<result_type>::waitWritev(
            m_state->writeIovecsData(),
            m_state->writeIovecsCount());
    }

    /// @brief 读取完成回调（写入状态机中无操作）
    void onRead(std::expected<size_t, IOError>)
    {
    }

    /// @brief 处理writev完成事件
    void onWrite(std::expected<size_t, IOError> result)
    {
        if (!result.has_value()) {
            m_state->setSendError(result.error());
            m_result = m_state->takeResult();
            return;
        }

        if (result.value() == 0) {
            m_state->onZeroWrite();
            m_result = m_state->takeResult();
            return;
        }

        m_state->onBytesWritten(result.value());
        if (m_state->isComplete()) {
            m_result = m_state->takeResult();
        }
    }

    std::shared_ptr<StateT> m_state;         ///< 状态对象
    std::optional<result_type> m_result;     ///< 缓存的最终结果
};

} // namespace detail

} // namespace galay::rpc

#endif // GALAY_RPC_AWAIT_H
