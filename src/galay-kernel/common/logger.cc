#include "galay-kernel/common/logger.h"

namespace galay::kernel::log
{

/**
 * @brief 设置 kernel 库自身 logger
 *
 * @details 转发到 KernelLoggerSlot，确保 kernel 日志与其他 galay 库日志隔离。
 *
 * @param logger 用户实现的日志实例，允许传入 nullptr 以禁用日志
 *
 * @note 线程不安全。不得与 get() 或其他 set() 并发调用。
 *       推荐在 main() 开头、创建任何 galay Runtime 之前调用。
 */
void set(BaseLogger::uptr logger)
{
    KernelLoggerSlot::set(std::move(logger));
}

/**
 * @brief 获取 kernel 库自身 logger
 *
 * @details 返回 KernelLoggerSlot 当前保存的 logger；未设置时返回 nullptr。
 *
 * @return 当前 logger 裸指针，或 nullptr
 *
 * @note 线程安全。可在任意线程、任意上下文中高频调用。
 * @note 返回的指针在 set() 被再次调用前保持有效。
 */
BaseLogger* get() noexcept
{
    return KernelLoggerSlot::get();
}

} // namespace galay::kernel::log
