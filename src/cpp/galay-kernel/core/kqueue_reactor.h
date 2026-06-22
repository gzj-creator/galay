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
#include "io_scheduler.hpp"
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
    KqueueReactor(int max_events, std::atomic<uint64_t>& last_error_code);  ///< 构造 kqueue reactor，并绑定错误输出槽位
    ~KqueueReactor();  ///< 释放 kqueue 与内部事件缓冲资源

    KqueueReactor(const KqueueReactor&) = delete;
    KqueueReactor& operator=(const KqueueReactor&) = delete;

    void notify();  ///< 从其他线程唤醒阻塞中的 kevent
    GHandle getHandle() const;  ///< 返回测试可见的 kqueue 句柄，用于观察唤醒事件
    std::expected<void, IOError> start();  ///< 显式初始化 kqueue 和唤醒事件，失败时返回 IOError

    int addAccept(IOController* controller);  ///< 注册 accept 等待；1=立即完成，0=已登记，<0=错误
    int addConnect(IOController* controller);  ///< 注册 connect 等待；1=立即完成，0=已登记，<0=错误
    int addRecv(IOController* controller);  ///< 注册 recv 等待；1=立即完成，0=已登记，<0=错误
    int addSend(IOController* controller);  ///< 注册 send 等待；1=立即完成，0=已登记，<0=错误
    int addReadv(IOController* controller);  ///< 注册 readv 等待；1=立即完成，0=已登记，<0=错误
    int addWritev(IOController* controller);  ///< 注册 writev 等待；1=立即完成，0=已登记，<0=错误
    int addClose(IOController* controller);  ///< 注册关闭操作；0=成功，<0=错误
    int addFileRead(IOController* controller);  ///< 注册文件读取等待；1=立即完成，0=已登记，<0=错误
    int addFileWrite(IOController* controller);  ///< 注册文件写入等待；1=立即完成，0=已登记，<0=错误
    int addRecvFrom(IOController* controller);  ///< 注册 recvfrom 等待；1=立即完成，0=已登记，<0=错误
    int addSendTo(IOController* controller);  ///< 注册 sendto 等待；1=立即完成，0=已登记，<0=错误
    int addFileWatch(IOController* controller);  ///< 注册文件监控等待；1=立即完成，0=已登记，<0=错误
    int addSendFile(IOController* controller);  ///< 注册 sendfile 等待；1=立即完成，0=已登记，<0=错误
    int addSequence(IOController* controller);  ///< 注册组合式序列等待；1=立即完成，0=已登记，<0=错误
    int remove(IOController* controller);  ///< 删除控制器相关的所有 kqueue 注册事件

    void poll(const struct timespec& timeout, WakeCoordinator& wake_coordinator);  ///< 轮询事件并通过 wake coordinator 分发唤醒

    /**
     * @brief 将 m_pending_changes 中的 kevent 提交到内核
     * @return 0 成功；-1 失败（已记录 lastError，失败的 batch 会保留待下次重试）
     * @note 当前运行时主要走立即注册，此接口保留给可选批量场景，并在每轮 follow-up 后尝试提交
     */
    int flushPendingChanges();

private:
    struct RegistrationEntry {
        IOController* controller = nullptr;
    };

    void processEvent(struct kevent& ev);  ///< 消费单个 kevent 事件并唤醒对应 awaitable
    int syncSequenceRegistration(IOController* controller);  ///< 同步 sequence awaitable 的注册状态
    int applySequenceInterest(IOController* controller, uint8_t desired_mask);  ///< 把 sequence 感兴趣的读写位应用到 kqueue
    RegistrationEntry* registrationEntryForController(IOController* controller);  ///< 获取 fd 对应的稳定注册入口
    void retireRegistrationEntry(IOController* controller);  ///< 退役 fd 对应注册入口，保留地址以过滤晚到事件

    static constexpr size_t BATCH_THRESHOLD = 32;  ///< 预留给批量注册场景的提交阈值
    static constexpr uintptr_t WAKE_IDENT = 1;  ///< 固定 EVFILT_USER 唤醒标识

    int m_kqueue_fd = -1;  ///< kqueue 描述符
    int m_max_events = 0;  ///< 单次 poll 处理的最大事件数
    std::vector<struct kevent> m_events;  ///< kevent 复用缓冲区
    std::vector<struct kevent> m_pending_changes;  ///< 待批量提交的 kevent 变更缓冲
    std::unordered_map<int, std::unique_ptr<RegistrationEntry>> m_registration_entries;  ///< fd 到稳定注册入口的映射
    std::vector<std::unique_ptr<RegistrationEntry>> m_retired_entries;  ///< 已退役但保留地址的注册入口
    std::atomic<uint64_t>& m_last_error_code;  ///< 最近一次后端错误编码输出槽位
};

static_assert(ReactorType<KqueueReactor>);

}  // namespace galay::kernel

#endif  // USE_KQUEUE

#endif  // GALAY_KERNEL_KQUEUE_REACTOR_H
