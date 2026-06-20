/**
 * @file etcd_log.h
 * @brief galay-etcd 独立日志入口与埋点宏
 */

#ifndef GALAY_ETCD_LOG_H
#define GALAY_ETCD_LOG_H

#include "../../galay-kernel/common/log_macro.h"

namespace galay::etcd::detail
{
struct EtcdLogTag;
} // namespace galay::etcd::detail

namespace galay::etcd::log
{
using Slot = ::galay::kernel::LoggerSlot<::galay::etcd::detail::EtcdLogTag>;

/**
 * @brief 设置 galay-etcd 的库级 logger
 *
 * @details 只影响 `ETCD_LOG_*` 宏产生的日志，不会启用 kernel、http
 * 或其他 galay 库日志。推荐在创建 etcd client 之前的单线程初始化阶段调用。
 *
 * @param logger 用户自定义 logger；传入 nullptr 时禁用 galay-etcd 日志。
 */
void set(::galay::kernel::BaseLogger::uptr logger);

/**
 * @brief 获取 galay-etcd 当前 logger
 *
 * @return 当前 logger 指针；未设置时返回 nullptr。
 *
 * @note 返回指针的生命周期由 `set()` 注入的 unique_ptr 管理，调用方不得释放。
 */
[[nodiscard]] ::galay::kernel::BaseLogger* get() noexcept;
} // namespace galay::etcd::log

/// @brief 判断指定级别的 galay-etcd 日志是否会实际写入
#define ETCD_LOG_ENABLED(level)                                                  \
    GALAY_LOG_ENABLED(::galay::etcd::log::get, level)

/// @brief galay-etcd 追踪日志宏，用于最详细的协议调试信息
#define ETCD_LOG_TRACE(tag, ...)                                                 \
    GALAY_LOG_WITH_LOGGER(::galay::etcd::log::get,                               \
                          ::galay::kernel::LogLevel::kTrace, "[etcd] " tag,      \
                          __VA_ARGS__)

/// @brief galay-etcd 调试日志宏，用于记录请求、响应和状态转换
#define ETCD_LOG_DEBUG(tag, ...)                                                 \
    GALAY_LOG_WITH_LOGGER(::galay::etcd::log::get,                               \
                          ::galay::kernel::LogLevel::kDebug, "[etcd] " tag,      \
                          __VA_ARGS__)

/// @brief galay-etcd 信息日志宏，用于记录连接和 watch 生命周期事件
#define ETCD_LOG_INFO(tag, ...)                                                  \
    GALAY_LOG_WITH_LOGGER(::galay::etcd::log::get,                               \
                          ::galay::kernel::LogLevel::kInfo, "[etcd] " tag,       \
                          __VA_ARGS__)

/// @brief galay-etcd 警告日志宏，用于表示协议错误、服务错误和可恢复异常
#define ETCD_LOG_WARN(tag, ...)                                                  \
    GALAY_LOG_WITH_LOGGER(::galay::etcd::log::get,                               \
                          ::galay::kernel::LogLevel::kWarn, "[etcd] " tag,       \
                          __VA_ARGS__)

/// @brief galay-etcd 错误日志宏，用于表示连接、读写或解析失败
#define ETCD_LOG_ERROR(tag, ...)                                                 \
    GALAY_LOG_WITH_LOGGER(::galay::etcd::log::get,                               \
                          ::galay::kernel::LogLevel::kError, "[etcd] " tag,      \
                          __VA_ARGS__)

#endif // GALAY_ETCD_LOG_H
