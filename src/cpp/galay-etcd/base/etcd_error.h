/**
 * @file etcd_error.h
 * @brief etcd 错误类型与错误类定义
 * @author galay-etcd
 * @version 1.0.0
 *
 * @details 定义 etcd 客户端操作中可能返回的错误类型枚举(EtcdErrorType)
 *          以及封装错误信息的 EtcdError 类。
 *          所有 etcd 操作的返回值通过 std::expected<T, EtcdError> 携带错误信息。
 */

#ifndef GALAY_ETCD_ERROR_H
#define GALAY_ETCD_ERROR_H

#include <string>

namespace galay::etcd
{

/**
 * @brief etcd 错误类型枚举
 * @details 描述 etcd 客户端操作中可能遇到的各种错误类别
 */
enum class EtcdErrorType
{
    Success = 0,        ///< 操作成功
    InvalidEndpoint,    ///< 无效的端点地址
    InvalidParam,       ///< 无效的参数
    NotConnected,       ///< 客户端未连接
    Connection,         ///< 连接失败
    Timeout,            ///< 操作超时
    Send,               ///< 发送数据失败
    Recv,               ///< 接收数据失败
    Http,               ///< HTTP 协议错误
    Server,             ///< etcd 服务端返回错误
    Parse,              ///< JSON 解析错误
    PoolExhausted,      ///< client 池暂无空闲 client
    Internal,           ///< 内部错误
};

/**
 * @brief etcd 错误类
 * @details 封装错误类型和附加错误消息，用于 std::expected 的错误通道
 */
class EtcdError
{
public:
    /**
     * @brief 构造一个指定类型的错误
     * @param type 错误类型，默认为 Success
     */
    EtcdError(EtcdErrorType type = EtcdErrorType::Success);

    /**
     * @brief 构造一个带附加消息的错误
     * @param type 错误类型
     * @param extra_msg 附加错误描述信息
     */
    EtcdError(EtcdErrorType type, std::string extra_msg);

    /**
     * @brief 获取错误类型
     * @return 错误类型枚举值
     */
    [[nodiscard]] EtcdErrorType type() const;

    /**
     * @brief 获取错误描述消息
     * @return 包含类型描述和附加消息的完整错误信息字符串
     */
    [[nodiscard]] std::string message() const;

    /**
     * @brief 判断是否为成功状态
     * @return 若错误类型为 Success 则返回 true
     */
    [[nodiscard]] bool isOk() const;

private:
    std::string m_extra_msg; ///< 附加错误消息
    EtcdErrorType m_type;    ///< 错误类型
};

} // namespace galay::etcd

#endif // GALAY_ETCD_ERROR_H
