/**
 * @file logger.h
 * @brief 日志抽象接口与按库隔离的注册槽
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 提供可插拔的日志基础设施。galay 系列库共享 BaseLogger 和
 * LogLevel 类型，但每个库通过自己的 log::set()/log::get() 持有独立 logger，
 * 从而支持用户只启用某一个库的日志。未设置 logger 时，埋点仅执行一次
 * atomic load + null 判断，不进入格式化。
 *
 * 使用方式：
 * @code
 * // 1. 用户实现 BaseLogger
 * class MyLogger : public galay::kernel::BaseLogger {
 * public:
 *     void log(LogLevel level, std::string_view tag,
 *              std::string_view message,
 *              const char* file, int line,
 *              const char* function) override {
 *         std::cout << std::format("[{}] {} {}:{} {}\n",
 *             levelToString(level), tag, file, line, message);
 *     }
 * };
 *
 * // 2. 只启用指定库日志
 * galay::kernel::log::set(std::make_unique<MyLogger>());
 *
 * // 3. 库内部埋点自动生效，无需其他操作
 * // 4. 若需重置，用户自行保证线程安全后再次调用 set()
 * @endcode
 */

#ifndef GALAY_KERNEL_LOGGER_H
#define GALAY_KERNEL_LOGGER_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>

namespace galay::kernel
{

/**
 * @brief 日志级别枚举
 *
 * @details 从低到高分为五个级别，用于过滤和分类日志消息。
 * 各库 log::get()->minLevel() 返回的值决定了哪些级别的消息会被实际发送到日志实现。
 */
enum class LogLevel : uint8_t
{
    kTrace = 0, ///< 最详细的追踪信息，通常仅用于开发调试
    kDebug,     ///< 调试信息，用于排查问题时提供上下文
    kInfo,      ///< 一般信息，记录程序运行的关键事件
    kWarn,      ///< 警告信息，表示潜在问题但不影响程序运行
    kError,     ///< 错误信息，表示操作失败或异常情况
};

/**
 * @brief 抽象日志接口
 *
 * @details 用户继承此类并实现 log() 方法，即可将 galay 系列库内部的
 * 日志埋点重定向到任意日志后端（控制台、文件、远程日志服务等）。
 *
 * 生命周期约定：
 * - 实例通过各库 log::set() 注入，由对应库的 LoggerSlot 持有所有权
 * - 设置后在程序整个生命周期内保持有效
 * - 如需替换或销毁，用户须保证在没有任何库代码并发调用 get() 时执行
 *
 * 线程安全要求：
 * - log() 实现必须是线程安全的，因为多个 IO 线程可能并发写入日志
 * - minLevel() 实现应返回不变量（constexpr 或从 immutable 状态读取）
 */
class BaseLogger
{
public:
    using uptr = std::unique_ptr<BaseLogger>;

    /**
     * @brief 析构函数
     * @details 虚析构以保证通过基类指针销毁派生类时正确调用派生类析构。
     */
    virtual ~BaseLogger() = default;

    /**
     * @brief 核心日志写入方法
     *
     * @details 当日志宏（GALAY_LOG_* 等）检测到 logger 已设置且消息级别
     * 不低于 minLevel() 时，将调用此方法。调用方已完成消息格式化，
     * 实现只需将消息路由到目标后端。
     *
     * @param level    本次消息的日志级别
     * @param tag      埋点标签，标识消息来源，格式为 "[模块] [子模块] [事件]"，
     *                 例如 "[http] [connect] [host:port]"、"[ssl] [handshake] [fail]"
     * @param message  经 std::format 格式化后的日志正文
     * @param file     产生此日志的源文件路径（编译期常量，由 __builtin_FILE() 提供）
     * @param line     产生此日志的源文件行号（编译期常量，由 __builtin_LINE() 提供）
     * @param function 产生此日志的函数名（编译期常量，由 __builtin_FUNCTION() 提供）
     *
     * @note 实现必须是线程安全的。galay 系列库使用多线程 IO 模型，
     *       此方法可能从任意调度器线程并发调用。
     * @note 建议实现尽量减少阻塞时间，避免影响 IO 吞吐。
     *       如需写入文件或网络，建议使用异步队列缓冲。
     */
    virtual void log(LogLevel level,
                     std::string_view tag,
                     std::string_view message,
                     const char* file,
                     int line,
                     const char* function) = 0;

    /**
     * @brief 获取此 logger 接受的最低日志级别
     *
     * @details 日志宏在格式化消息之前会先检查此方法返回值，
     * 低于此级别的消息不会触发 log() 调用，也不会执行 std::format，
     * 从而实现零开销过滤。
     *
     * @return 最低日志级别，默认 kTrace（接收所有级别）
     *
     * @note 此方法应返回不变量。如果需要动态调整日志级别，
     *       返回值应使用 std::atomic 或由用户保证线程安全。
     */
    virtual LogLevel minLevel() const { return LogLevel::kTrace; }
};

/**
 * @brief 按标签隔离的 logger 存储槽
 *
 * @details 每个库应定义一个唯一 tag 类型，并使用 LoggerSlot<Tag> 作为该库
 * log::set()/log::get() 的后端。不同 tag 的槽位拥有独立 logger，
 * 因此用户只设置某个库 logger 时不会启用其他库日志。
 *
 * 所有权模型：
 * - set() 接受 std::unique_ptr<BaseLogger>，取得 logger 的所有权
 * - 内部通过 inline static unique_ptr 持有，程序退出时自动析构
 *
 * 线程安全模型：
 * - get() 是线程安全的，使用 atomic acquire 语义，可在任意线程调用
 * - set() 是线程不安全的，用户须在程序初始化阶段（单线程环境）调用一次
 * - 若需在运行时替换 logger，用户须自行保证 set() 与 get() 的同步
 */
template <typename Tag>
class LoggerSlot
{
public:
    /**
     * @brief 设置当前槽位的 logger
     *
     * @details 将 logger 存入当前 tag 对应的槽位并取得所有权。
     * 调用后该槽位的 get() 将返回新设置的 logger，其他 tag 不受影响。
     *
     * @param logger 用户实现的日志实例，通过 unique_ptr 传入以转移所有权。
     *               传入 nullptr 等价于禁用日志（get() 返回 nullptr）。
     *
     * @note 线程不安全。不得与 get() 并发调用。
     *       推荐在 main() 开头、创建任何 galay 对象之前调用。
     */
    static void set(BaseLogger::uptr logger)
    {
        m_owned = std::move(logger);
        m_instance.store(m_owned.get(), std::memory_order_release);
    }

    /**
     * @brief 获取当前槽位的 logger
     *
     * @details 返回通过 set() 设置的 logger 裸指针，未设置时返回 nullptr。
     * 使用 atomic acquire 语义，保证看到 set() 中 store 的完整 BaseLogger 对象。
     *
     * @return 当前 logger 指针，或 nullptr（未设置时）
     *
     * @note 线程安全。可在任意线程、任意上下文中调用。
     * @note 返回的指针在 set() 被再次调用或程序退出前保持有效。
     */
    [[nodiscard]] static BaseLogger* get() noexcept
    {
        return m_instance.load(std::memory_order_acquire);
    }

    /**
     * @brief 禁止实例化
     * @details LoggerSlot 仅提供静态方法，不允许创建实例。
     */
    LoggerSlot() = delete;

private:
    static inline BaseLogger::uptr m_owned{};
    static inline std::atomic<BaseLogger*> m_instance{nullptr};
};

namespace detail
{
struct KernelLogTag;
} // namespace detail

/**
 * @brief kernel 库自身使用的 logger 槽位
 *
 * @details 下游库不得复用此槽位，应在各自命名空间内定义独立 tag 和 log::set/get。
 */
using KernelLoggerSlot = LoggerSlot<detail::KernelLogTag>;

namespace log
{
/**
 * @brief 设置 galay-kernel 自身 logger
 *
 * @details 只影响 `GALAY_KERNEL_LOG_*` 宏产生的 kernel 日志，不会启用其他 galay 库日志。
 *
 * @param logger 用户实现的 logger；传入 nullptr 时禁用 kernel 日志。
 */
void set(BaseLogger::uptr logger);

/**
 * @brief 获取 galay-kernel 自身 logger
 *
 * @return 当前 kernel logger 指针；未设置时返回 nullptr。
 */
[[nodiscard]] BaseLogger* get() noexcept;
} // namespace log

} // namespace galay::kernel

#endif // GALAY_KERNEL_LOGGER_H
