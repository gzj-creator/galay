/**
 * @file error.h
 * @brief 统一 I/O 错误码与错误报告
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 定义内核级 I/O 错误的 IOErrorCode 枚举以及 IOError 类，
 * IOError 将 IOErrorCode 和平台系统错误（errno/WSAGetLastError）
 * 打包为单个 64 位值。同时定义 Infallible，用于不可能失败的操作的标记类型。
 */

#ifndef GALAY_KERNEL_ERROR_H
#define GALAY_KERNEL_ERROR_H

#include <cstdint>
#include <string>

namespace galay::kernel
{

/**
 * @brief 统一的 galay-kernel I/O 错误码
 */
enum IOErrorCode : uint32_t {
    kDisconnectError = 0,  ///< 连接断开或关闭失败
    kNotReady,             ///< 资源未就绪或当前状态不允许操作
    kParamInvalid,         ///< 无效参数
    kRecvFailed,           ///< 接收数据失败
    kSendFailed,           ///< 发送数据失败
    kAcceptFailed,         ///< 接受新连接失败
    kConnectFailed,        ///< 连接远程主机失败
    kBindFailed,           ///< 绑定地址失败
    kListenFailed,         ///< 监听套接字失败
    kOpenFailed,           ///< 打开文件失败
    kReadFailed,           ///< 读取失败
    kWriteFailed,          ///< 写入失败
    kStatFailed,           ///< 查询文件状态失败
    kSyncFailed,           ///< 同步文件到磁盘失败
    kSeekFailed,           ///< 定位文件偏移失败
    kTimeout,              ///< 操作超时
    kNotRunningOnIOScheduler  ///< 未在 I/O 调度器上运行
};

/**
* @brief 通用 I/O 错误类
* @details 将 IOErrorCode（低 32 位）和系统错误码（高 32 位）
*          打包为单个 64 位值。提供人类可读的错误消息。
*/
class IOError
{
public:
    /**
    * @brief 检查打包的错误码是否包含指定的 IOErrorCode
    * @param error 打包的 64 位错误值
    * @param code 要测试的 IOErrorCode
    * @return 若低 32 位匹配则返回 true
    */
    static bool contains(uint64_t error, IOErrorCode code);

    /**
    * @brief 从 IOErrorCode 和系统错误码构造
    * @param io_error_code 内核级错误类别
    * @param system_code 平台错误（errno / WSAGetLastError）
    */
    IOError(IOErrorCode io_error_code, uint32_t system_code);

    /**
    * @brief 获取打包的 64 位错误码
    * @return 错误码（高 32 位：系统错误，低 32 位：I/O 错误）
    */
    uint64_t code() const;

    /**
    * @brief 获取人类可读的错误消息
    * @return 结合 I/O 描述和系统 strerror 的字符串
    */
    std::string message() const;

    /**
    * @brief 重置为无错误状态（code = 0）
    */
    void reset();
private:
    /**
    * @brief 将 IOErrorCode 和系统错误码打包为单个 64 位值
    * @param io_error_code 低 32 位
    * @param system_code 高 32 位
    * @return 打包的 64 位错误码
    */
    uint64_t makeErrorCode(IOErrorCode io_error_code, uint32_t system_code);
private:
    uint64_t m_code;  ///< 打包的错误码
};

/**
* @brief 不可能失败的操作的标记类型
*/
class Infallible
{
};

}

#endif
