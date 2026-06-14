/**
 * @file backend_reactor.h
 * @brief 平台无关的 IO 事件后端 reactor 抽象
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 定义所有具体 IO 后端（epoll、kqueue、io_uring）必须实现的 BackendReactor 接口，
 * 同时提供用于在原子槽位中存储和加载后端错误码的辅助函数。
 */

#ifndef GALAY_KERNEL_BACKEND_REACTOR_H
#define GALAY_KERNEL_BACKEND_REACTOR_H

#include "kernel/common/error.h"

#include <atomic>
#include <optional>

namespace galay::kernel {

/**
 * @brief 平台后端 reactor 抽象基类
 * @details 由 epoll、kqueue、io_uring 等具体后端实现，用于向调度器提供统一唤醒接口。
 */
class BackendReactor
{
public:
    virtual ~BackendReactor() = default;  ///< 虚析构，允许通过基类指针安全释放具体后端
    virtual void notify() = 0;  ///< 从其他线程唤醒阻塞中的 reactor
    virtual int wakeReadFdForTest() const = 0;  ///< 返回测试可见的唤醒读端句柄；不可用时返回负值
};

namespace detail {

/**
 * @brief 以统一编码格式保存后端内部错误
 * @param last_error_code 目标原子错误码槽位
 * @param error_code 框架级错误码
 * @param system_code 系统调用错误码
 */
inline void storeBackendError(std::atomic<uint64_t>& last_error_code,
                              IOErrorCode error_code,
                              uint32_t system_code) noexcept {
    last_error_code.store(IOError(error_code, system_code).code(), std::memory_order_release);
}

/**
 * @brief 读取后端最近一次内部错误
 * @param last_error_code 保存编码后错误的原子变量
 * @return 存在错误时返回 `IOError`，否则返回 `std::nullopt`
 */
inline auto loadBackendError(const std::atomic<uint64_t>& last_error_code)
    -> std::optional<IOError> {
    const uint64_t code = last_error_code.load(std::memory_order_acquire);
    if (code == 0) {
        return std::nullopt;
    }
    return IOError(static_cast<IOErrorCode>(code & 0xffffffffu),
                   static_cast<uint32_t>(code >> 32));
}

}  // namespace detail

}  // namespace galay::kernel

#endif  // GALAY_KERNEL_BACKEND_REACTOR_H
