/**
 * @file kqueue_reactor.h
 * @brief 基于 macOS/BSD kqueue 的 IO reactor
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 使用 BSD kqueue 实现事件通知、EVFILT_USER 跨线程唤醒、
 * EVFILT_VNODE 文件系统监控。使用稳定的 RegistrationEntry 指针作为 kevent udata，
 * 安全处理 fd 复用和控制器迁移。
 */

#ifndef GALAY_KERNEL_KQUEUE_REACTOR_H
#define GALAY_KERNEL_KQUEUE_REACTOR_H

#include "backend_reactor.h"
#include "io_controller.hpp"
#include "wake_coordinator.h"

#ifdef USE_KQUEUE

#include <cstdint>
#include <sys/event.h>

#include <atomic>
#include <expected>
#include <memory>
#include <unordered_map>
#include <vector>

namespace galay::kernel {

/**
 * @brief kqueue 后端 reactor
 * @details 负责 macOS/BSD 上 kqueue 事件的注册、唤醒与分发。
 */
class KqueueReactor
{
public:
    /**
     * @brief 构造 kqueue reactor，并绑定错误输出槽位
     * @param max_events 最多处理的事件数量
     * @param last_error_code 最近一次错误码
     */
    KqueueReactor(int max_events, std::atomic<uint64_t>& last_error_code);
    ~KqueueReactor();  ///< 释放 kqueue 与内部事件缓冲资源

    KqueueReactor(const KqueueReactor&) = delete;
    KqueueReactor& operator=(const KqueueReactor&) = delete;

    /**
     * @brief 从其他线程唤醒阻塞中的 kevent
     * @return 无返回值
     */
    void notify();
    /**
     * @brief 返回测试可见的 kqueue 句柄，用于观察唤醒事件
     * @return GHandle 操作结果
     */
    GHandle get_handle() const;
    /**
     * @brief 显式初始化 kqueue 和唤醒事件，失败时返回 IOError
     * @return 成功时返回空值，失败时返回 IOError 错误
     */
    std::expected<void, IOError> start();

    /**
     * @brief 注册 accept 等待；1=立即完成，0=已登记，<0=错误
     * @param controller IO 控制器
     * @return int 操作结果
     */
    int add_accept(IOController* controller);
    /** @brief Complete and detach accepts when the owning scheduler stops. */
    void stop_accepts();
    /**
     * @brief 注册 connect 等待；1=立即完成，0=已登记，<0=错误
     * @param controller IO 控制器
     * @return int 操作结果
     */
    int add_connect(IOController* controller);
    /**
     * @brief 注册 recv 等待；1=立即完成，0=已登记，<0=错误
     * @param controller IO 控制器
     * @return int 操作结果
     */
    int add_recv(IOController* controller);
    /**
     * @brief 注册 send 等待；1=立即完成，0=已登记，<0=错误
     * @param controller IO 控制器
     * @return int 操作结果
     */
    int add_send(IOController* controller);
    /**
     * @brief 注册 readv 等待；1=立即完成，0=已登记，<0=错误
     * @param controller IO 控制器
     * @return int 操作结果
     */
    int add_readv(IOController* controller);
    /**
     * @brief 注册 writev 等待；1=立即完成，0=已登记，<0=错误
     * @param controller IO 控制器
     * @return int 操作结果
     */
    int add_writev(IOController* controller);
    /**
     * @brief 注册关闭操作；0=成功，<0=错误
     * @param controller IO 控制器
     * @return int 操作结果
     */
    int add_close(IOController* controller);
    /**
     * @brief 注册文件读取等待；1=立即完成，0=已登记，<0=错误
     * @param controller IO 控制器
     * @return int 操作结果
     */
    int add_file_read(IOController* controller);
    /**
     * @brief 注册文件写入等待；1=立即完成，0=已登记，<0=错误
     * @param controller IO 控制器
     * @return int 操作结果
     */
    int add_file_write(IOController* controller);
    /**
     * @brief 注册 recvfrom 等待；1=立即完成，0=已登记，<0=错误
     * @param controller IO 控制器
     * @return int 操作结果
     */
    int add_recv_from(IOController* controller);
    /**
     * @brief 注册 sendto 等待；1=立即完成，0=已登记，<0=错误
     * @param controller IO 控制器
     * @return int 操作结果
     */
    int add_send_to(IOController* controller);
    /**
     * @brief 注册文件监控等待；1=立即完成，0=已登记，<0=错误
     * @param controller IO 控制器
     * @return int 操作结果
     */
    int add_file_watch(IOController* controller);
    /**
     * @brief 注册 sendfile 等待；1=立即完成，0=已登记，<0=错误
     * @param controller IO 控制器
     * @return int 操作结果
     */
    int add_send_file(IOController* controller);
    /**
     * @brief 注册组合式序列等待；1=立即完成，0=已登记，<0=错误
     * @param controller IO 控制器
     * @return int 操作结果
     */
    int add_sequence(IOController* controller);
    /**
     * @brief 删除控制器相关的所有 kqueue 注册事件
     * @param controller IO 控制器
     * @return int 操作结果
     */
    int remove(IOController* controller);

    /**
     * @brief 轮询事件并通过 wake coordinator 分发唤醒
     * @param timeout 超时时长
     * @param wake_coordinator 唤醒协调器
     * @return 无返回值
     */
    void poll(const struct timespec& timeout, WakeCoordinator& wake_coordinator);

    /**
     * @brief 将 m_pending_changes 中的 kevent 提交到内核
     * @return 0 成功；-1 失败（已记录 last_error，失败的 batch 会保留待下次重试）
     * @note 简单 awaitable 的注册变更先进入 batch；sequence 仍同步提交以保持时序
     */
    int flush_pending_changes();

private:
    struct RegistrationEntry {
        IOController* controller = nullptr;
    };

    /**
     * @brief 消费单个 kevent 事件并唤醒对应 awaitable
     * @param ev 事件对象
     * @return 无返回值
     */
    void process_event(struct kevent& ev);
    /**
     * @brief 更新普通 awaitable 的逻辑兴趣位，并按需缓存物理 kevent 变更
     * @param controller IO 控制器
     * @param slot 队列槽位
     * @param desired 目标状态
     * @return int 操作结果
     */
    int update_simple_interest(IOController* controller,
                             IOController::Index slot,
                             bool desired);
    /**
     * @brief 删除一次性注册并把非 ENOENT 错误写入 last_error
     * @param fd 文件描述符
     * @param filter 事件过滤类型
     * @return 无返回值
     */
    void delete_one_shot_registration(int fd, int16_t filter);
    /**
     * @brief 丢弃指定 controller 尚未提交的注册变更
     * @param controller IO 控制器
     * @return 无返回值
     */
    void discard_pending_changes(IOController* controller);
    /**
     * @brief 同步 sequence awaitable 的注册状态
     * @param controller IO 控制器
     * @return int 操作结果
     */
    int sync_sequence_registration(IOController* controller);
    /**
     * @brief 把 sequence 感兴趣的读写位应用到 kqueue
     * @param controller IO 控制器
     * @param desired_mask 目标事件掩码
     * @return int 操作结果
     */
    int apply_sequence_interest(IOController* controller, uint8_t desired_mask);
    /**
     * @brief 获取 fd 对应的稳定注册入口
     * @param controller IO 控制器
     * @return RegistrationEntry* 指针
     */
    RegistrationEntry* registration_entry_for_controller(IOController* controller);
    /**
     * @brief 退役 fd 对应注册入口，保留地址以过滤晚到事件
     * @param controller IO 控制器
     * @return 无返回值
     */
    void retire_registration_entry(IOController* controller);

    static constexpr size_t BATCH_THRESHOLD = 32;  ///< 达到该数量时提前提交，避免注册延迟无界增长
    static constexpr uintptr_t WAKE_IDENT = 1;  ///< 固定 EVFILT_USER 唤醒标识

    int m_kqueue_fd = -1;  ///< kqueue 描述符
    int m_max_events = 0;  ///< 单次 poll 处理的最大事件数
    std::vector<struct kevent> m_events;  ///< kevent 复用缓冲区
    std::vector<struct kevent> m_pending_changes;  ///< 待批量提交的 kevent 变更缓冲
    std::unordered_map<int, std::unique_ptr<RegistrationEntry>> m_registration_entries;  ///< fd 到稳定注册入口的映射
    std::vector<std::unique_ptr<RegistrationEntry>> m_retired_entries;  ///< 已退役但保留地址的注册入口
    std::atomic<uint64_t>& m_last_error_code;  ///< 最近一次后端错误编码输出槽位
    bool m_accept_stopping = false;
};

static_assert(ReactorType<KqueueReactor>);

}  // namespace galay::kernel

#endif  // USE_KQUEUE

#endif  // GALAY_KERNEL_KQUEUE_REACTOR_H
