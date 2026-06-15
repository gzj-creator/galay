/**
 * @file rpc_log.h
 * @brief galay-rpc 独立日志入口与埋点宏
 * @author galay-rpc
 * @version 1.0.0
 *
 * @details 提供galay-rpc库专用的日志槽位和日志宏。
 *          只影响 RPC_LOG_* 宏产生的日志，不会启用 kernel、ssl、http
 *          或其他 galay 库的日志。
 */

#ifndef GALAY_RPC_LOG_H
#define GALAY_RPC_LOG_H

#include "galay-kernel/common/log_macro.h"

namespace galay::rpc::detail
{
/// @brief RPC日志标签类型，用于区分RPC日志与其他库日志
struct RpcLogTag;
} // namespace galay::rpc::detail

namespace galay::rpc::log
{
/// @brief RPC日志槽位类型，绑定到RpcLogTag的LoggerSlot实例
using Slot = ::galay::kernel::LoggerSlot<::galay::rpc::detail::RpcLogTag>;

/**
 * @brief 设置 galay-rpc 的库级 logger
 *
 * @details 只影响 `RPC_LOG_*` 宏产生的日志，不会启用 kernel、ssl、http
 * 或其他 galay 库的日志。推荐在创建 RpcClient/RpcServer 之前的单线程初始化阶段调用。
 *
 * @param logger 用户自定义 logger；传入 nullptr 时禁用 galay-rpc 日志。
 */
inline void set(::galay::kernel::BaseLogger::uptr logger)
{
    Slot::set(std::move(logger));
}

/**
 * @brief 获取 galay-rpc 当前 logger
 *
 * @return 当前 logger 指针；未设置时返回 nullptr。
 *
 * @note 返回指针的生命周期由 `set()` 注入的 unique_ptr 管理，调用方不得释放。
 */
[[nodiscard]] inline ::galay::kernel::BaseLogger* get() noexcept
{
    return Slot::get();
}
} // namespace galay::rpc::log

/// @brief 判断指定级别的 galay-rpc 日志是否会实际写入
#define RPC_LOG_ENABLED(level)                                                   \
    GALAY_LOG_ENABLED(::galay::rpc::log::get, level)

/// @brief galay-rpc 追踪日志宏
#define RPC_LOG_TRACE(tag, ...)                                                  \
    GALAY_LOG_WITH_LOGGER(::galay::rpc::log::get,                                \
                          ::galay::kernel::LogLevel::kTrace, "[rpc] " tag,       \
                          __VA_ARGS__)

/// @brief galay-rpc 调试日志宏
#define RPC_LOG_DEBUG(tag, ...)                                                  \
    GALAY_LOG_WITH_LOGGER(::galay::rpc::log::get,                                \
                          ::galay::kernel::LogLevel::kDebug, "[rpc] " tag,       \
                          __VA_ARGS__)

/// @brief galay-rpc 信息日志宏
#define RPC_LOG_INFO(tag, ...)                                                   \
    GALAY_LOG_WITH_LOGGER(::galay::rpc::log::get,                                \
                          ::galay::kernel::LogLevel::kInfo, "[rpc] " tag,        \
                          __VA_ARGS__)

/// @brief galay-rpc 警告日志宏
#define RPC_LOG_WARN(tag, ...)                                                   \
    GALAY_LOG_WITH_LOGGER(::galay::rpc::log::get,                                \
                          ::galay::kernel::LogLevel::kWarn, "[rpc] " tag,        \
                          __VA_ARGS__)

/// @brief galay-rpc 错误日志宏
#define RPC_LOG_ERROR(tag, ...)                                                  \
    GALAY_LOG_WITH_LOGGER(::galay::rpc::log::get,                                \
                          ::galay::kernel::LogLevel::kError, "[rpc] " tag,       \
                          __VA_ARGS__)

#endif // GALAY_RPC_LOG_H
