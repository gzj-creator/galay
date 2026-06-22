/**
 * @file epoll_reactor.h
 * @brief 基于 Linux epoll 的 IO reactor
 * @author galay-kernel
 * @version 1.0.0
 *
 * @details 使用 Linux epoll、eventfd、inotify 和 libaio 满足 ReactorType concept。
 * 通过待提交变更队列批量处理事件注册以减少系统调用。
 */

#ifndef GALAY_KERNEL_EPOLL_REACTOR_H
#define GALAY_KERNEL_EPOLL_REACTOR_H

#include "backend_reactor.h"
#include "io_scheduler.hpp"
#include "wake_coordinator.h"

#ifdef USE_EPOLL

#include <sys/epoll.h>

#include <atomic>
#include <expected>
#include <memory>
#include <unordered_map>
#include <vector>

namespace galay::kernel {

/**
 * @brief epoll 后端 reactor
 * @details 负责 Linux 上 epoll/eventfd/inotify/libaio 事件的注册、唤醒与分发。
 */
class EpollReactor
{
public:
    EpollReactor(int max_events, std::atomic<uint64_t>& last_error_code);  ///< 构造 epoll reactor，并绑定错误输出槽位
    ~EpollReactor();  ///< 释放 epoll/eventfd 等底层资源

    EpollReactor(const EpollReactor&) = delete;
    EpollReactor& operator=(const EpollReactor&) = delete;

    void notify();  ///< 从其他线程唤醒阻塞中的 epoll_wait
    GHandle getHandle() const;  ///< 返回测试可见的 eventfd 读端句柄
    std::expected<void, IOError> start();  ///< 显式初始化 epoll 和 eventfd，失败时返回 IOError

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
    int remove(IOController* controller);  ///< 删除控制器相关的所有 epoll 注册事件
    int flushPendingChanges();  ///< 把本地 pending 注册/反注册请求批量提交到内核

    void poll(int timeout_ms, WakeCoordinator& wake_coordinator);  ///< 轮询事件并通过 wake coordinator 分发唤醒

private:
    struct RegistrationEntry {
        IOController* controller = nullptr;  ///< 当前 fd 绑定的控制器；退役后置空过滤晚到事件
    };

    struct PendingChange {
        IOController* controller = nullptr;  ///< 对应控制器
        uint32_t events = EPOLLET;  ///< 目标事件掩码；仅 EPOLLET 表示删除注册
    };

    uint32_t buildEvents(IOController* controller) const;  ///< 根据控制器状态计算目标 epoll 事件掩码
    int applyEvents(IOController* controller, uint32_t events);  ///< 把计算出的 epoll 事件掩码写入本地 pending 队列
    int processSequence(IOEventType type, IOController* controller);  ///< 处理 sequence awaitable 的注册/同步逻辑
    void processEvent(struct epoll_event& ev);  ///< 消费单个 epoll 事件并唤醒对应 awaitable
    void syncEvents(IOController* controller);  ///< 同步控制器当前关注事件到 epoll
    size_t findPendingChangeIndex(IOController* controller) const;  ///< 查找控制器对应的 pending change
    void erasePendingChange(size_t index);  ///< 删除指定下标的 pending change
    void discardPendingChange(IOController* controller);  ///< 丢弃控制器对应的 pending change
    RegistrationEntry* registrationEntryForController(IOController* controller);  ///< 获取 fd 对应的稳定注册入口
    void retireRegistrationEntry(IOController* controller);  ///< 退役 fd 对应注册入口，保留地址过滤晚到事件

    static constexpr size_t BATCH_THRESHOLD = 32;  ///< 累积到一定数量时主动 flush，避免队列无限增长

    int m_epoll_fd = -1;  ///< epoll 实例 fd
    int m_event_fd = -1;  ///< 跨线程唤醒用 eventfd
    int m_max_events = 0;  ///< 单次 poll 处理的最大事件数
    std::vector<struct epoll_event> m_events;  ///< epoll_wait 复用缓冲区
    std::vector<PendingChange> m_pending_changes;  ///< 待批量提交的 epoll 事件变更
    std::unordered_map<int, std::unique_ptr<RegistrationEntry>> m_registration_entries;  ///< fd 到稳定注册入口的映射
    std::vector<std::unique_ptr<RegistrationEntry>> m_retired_entries;  ///< 已退役但保留地址的注册入口
    std::atomic<uint64_t>& m_last_error_code;  ///< 最近一次后端错误编码输出槽位
};

static_assert(ReactorType<EpollReactor>);

}  // namespace galay::kernel

#endif  // USE_EPOLL

#endif  // GALAY_KERNEL_EPOLL_REACTOR_H
