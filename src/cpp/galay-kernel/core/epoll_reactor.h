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
#include "io_controller.hpp"
#include "wake_coordinator.h"

#ifdef USE_EPOLL
#include "operation_completion.hpp"

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
    /**
     * @brief 构造 epoll reactor，并绑定错误输出槽位
     * @param max_events 最多处理的事件数量
     * @param last_error_code 最近一次错误码
     */
    EpollReactor(int max_events, std::atomic<uint64_t>& last_error_code);
    ~EpollReactor();  ///< 释放 epoll/eventfd 等底层资源

    EpollReactor(const EpollReactor&) = delete;
    EpollReactor& operator=(const EpollReactor&) = delete;

    /**
     * @brief 从其他线程唤醒阻塞中的 epoll_wait
     * @return 无返回值
     */
    void notify();
    /**
     * @brief 返回测试可见的 eventfd 读端句柄
     * @return GHandle 操作结果
     */
    GHandle get_handle() const;
    /**
     * @brief 返回 epoll 实例句柄，供派生 scheduler 的后端诊断使用
     * @return GHandle 操作结果
     */
    GHandle get_poll_handle() const;
    /**
     * @brief 显式初始化 epoll 和 eventfd，失败时返回 IOError
     * @return 成功时返回空值，失败时返回 IOError 错误
     */
    std::expected<void, IOError> start();

    /**
     * @brief 注册 accept 等待；1=立即完成，0=已登记，<0=错误
     * @param controller IO 控制器
     * @return int 操作结果
     */
    int add_accept(IOController* controller);
    /**
     * @brief 构造最终地址的 accept operation 并提交；false 为同步完成。
     * @param awaitable 等待体
     * @param waker 协程唤醒器
     * @return 操作的布尔结果，具体条件见函数说明
     */
    bool submit_accept(AcceptAwaitable& awaitable, Waker&& waker);
    /**
     * @brief 先清除注册/slot，再移交恢复权；错误通过 last_error 记录。
     * @param awaitable 等待体
     * @return 成功时返回 ResumeCapability，失败时返回 OperationError 错误
     */
    std::expected<ResumeCapability, OperationError> detach_accept(AcceptAwaitable& awaitable);
    /**
     * @brief Owner 上冻结 timeout 结果并解除注册，最后才恢复等待者。
     * @param awaitable 等待体
     * @return 无返回值
     */
    void timeout_accept(AcceptAwaitable& awaitable);
    /**
     * @brief 停机 owner 入口：拒绝新 accept，完成并解绑所有已登记 accept。
     * @return 无返回值
     */
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
     * @brief 删除控制器相关的所有 epoll 注册事件
     * @param controller IO 控制器
     * @return int 操作结果
     */
    int remove(IOController* controller);
    /**
     * @brief 把本地 pending 注册/反注册请求批量提交到内核
     * @return int 操作结果
     */
    int flush_pending_changes();

    /**
     * @brief 轮询事件并通过 wake coordinator 分发唤醒
     * @param timeout_ms 超时时间，单位为毫秒
     * @param wake_coordinator 唤醒协调器
     * @return 无返回值
     */
    void poll(int timeout_ms, WakeCoordinator& wake_coordinator);

private:
    friend struct EpollReactorTestAccess; // 无布局/分支成本；测试只注入已拷贝 ready event。
    struct RegistrationEntry {
        IOController* controller = nullptr;  ///< 当前 fd 绑定的控制器；退役后置空过滤晚到事件
    };

    struct PendingChange {
        RegistrationEntry* entry = nullptr;  ///< 稳定注册入口；controller 移动后仍可解析当前 owner
        uint32_t events = EPOLLET;  ///< 目标事件掩码；仅 EPOLLET 表示删除注册
    };

    /**
     * @brief 根据控制器状态计算目标 epoll 事件掩码
     * @param controller IO 控制器
     * @return uint32_t 操作结果
     */
    uint32_t build_events(IOController* controller) const;
    /**
     * @brief 为 recv/readv 保留持久 EPOLLET READ 兴趣
     * @param controller IO 控制器
     * @return int 操作结果
     */
    int arm_persistent_read(IOController* controller);
    /**
     * @brief 为 send 保留持久 EPOLLET WRITE 兴趣
     * @param controller IO 控制器
     * @return int 操作结果
     */
    int arm_persistent_write(IOController* controller);
    /**
     * @brief 排队变更；accept 提交期间禁止派发其他操作的错误恢复
     * @param controller IO 控制器
     * @param events 事件数组
     * @param flush_at_threshold 达到阈值时是否提交
     * @return int 操作结果
     */
    int apply_events(IOController* controller, uint32_t events, bool flush_at_threshold = true);
    /**
     * @brief 只提交此资源的变更；不派发任何恢复回调
     * @param controller IO 控制器
     * @param events 事件数组
     * @return int 操作结果
     */
    int update_registration(IOController* controller, uint32_t events);
    /**
     * @brief 处理 sequence awaitable 的注册/同步逻辑
     * @param type 类型
     * @param controller IO 控制器
     * @return int 操作结果
     */
    int process_sequence(IOEventType type, IOController* controller);
    /**
     * @brief 消费单个 epoll 事件并唤醒对应 awaitable
     * @param ev 事件对象
     * @return 无返回值
     */
    void process_event(struct epoll_event& ev);
    /**
     * @brief 同步控制器当前关注事件到 epoll
     * @param controller IO 控制器
     * @return 无返回值
     */
    void sync_events(IOController* controller);
    /**
     * @brief 同时返回 pending 下标与已确认归属的稳定入口
     * @param controller IO 控制器
     * @return std::pair<size_t, RegistrationEntry*> 操作结果
     */
    std::pair<size_t, RegistrationEntry*> find_pending_change(IOController* controller) const;
    /**
     * @brief 删除指定下标的 pending change
     * @param index 元素索引
     * @return 无返回值
     */
    void erase_pending_change(size_t index);
    /**
     * @brief 丢弃控制器对应的 pending change
     * @param controller IO 控制器
     * @return 无返回值
     */
    void discard_pending_change(IOController* controller);
    /**
     * @brief 获取 fd 对应的稳定注册入口
     * @param controller IO 控制器
     * @return RegistrationEntry* 指针
     */
    RegistrationEntry* registration_entry_for_controller(IOController* controller);
    /**
     * @brief 退役 fd 对应注册入口，保留地址过滤晚到事件
     * @param controller IO 控制器
     * @return 无返回值
     */
    void retire_registration_entry(IOController* controller);

    static constexpr size_t BATCH_THRESHOLD = 32;  ///< 累积到一定数量时主动 flush，避免队列无限增长

    int m_epoll_fd = -1;  ///< epoll 实例 fd
    int m_event_fd = -1;  ///< 跨线程唤醒用 eventfd
    int m_max_events = 0;  ///< 单次 poll 处理的最大事件数
    std::vector<struct epoll_event> m_events;  ///< epoll_wait 复用缓冲区
    std::vector<PendingChange> m_pending_changes;  ///< 待批量提交的 epoll 事件变更
    std::unordered_map<RegistrationEntry*, size_t> m_pending_change_index;  ///< O(1) 查找稳定注册入口对应的 pending change 索引
    std::unordered_map<int, std::unique_ptr<RegistrationEntry>> m_registration_entries;  ///< fd 到稳定注册入口的映射
    std::vector<std::unique_ptr<RegistrationEntry>> m_retired_entries;  ///< 已退役但保留地址的注册入口
    std::atomic<uint64_t>& m_last_error_code;  ///< 最近一次后端错误编码输出槽位
    uint32_t m_next_accept_generation = 1; ///< 耗尽时拒绝提交，绝不重用旧 key。
    bool m_accept_stopping = false;
};

static_assert(ReactorType<EpollReactor>);

}  // namespace galay::kernel

#endif  // USE_EPOLL

#endif  // GALAY_KERNEL_EPOLL_REACTOR_H
